#include "ShaderBytecodeRecord.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "globals.h"
#include "log.h"

#define SHADER_BYTECODE_RECORD_CACHE_MAGIC ('S' | ('B' << 8) | ('R' << 16) | ('C' << 24))
#define SHADER_BYTECODE_RECORD_CACHE_VERSION 1

// Cap on the number of records kept. The ledger is generation-independent so
// it grows across ShaderRegex reloads; when the cap is exceeded the oldest
// (by last_seen) records are pruned. Their blob blocks stay behind in the
// grow-only blob file (dead blocks are skipped over by record offsets).
// The macro indirection lets the test harness shrink the cap so the prune
// path is exercisable without a 16k-record ledger on disk:
#ifndef SHADER_BYTECODE_RECORD_MAX_RECORDS
#define SHADER_BYTECODE_RECORD_MAX_RECORDS 16384
#endif
static constexpr size_t SHADER_BYTECODE_RECORD_MAX = SHADER_BYTECODE_RECORD_MAX_RECORDS;

// Similar in spirit to ShaderRegexCacheStore, but this ledger stores the
// *original* (unpatched) bytecode, keyed only by (stage, hash). It survives
// ShaderRegex generation wipes so the async pre-deferred analysis workers can
// always get the original bytecode for a shader even after the regex cache
// was regenerated, and it also feeds frame analysis.
class ShaderBytecodeRecordStore {
public:
	bool load(UINT64 hash, const wchar_t *shader_type, std::vector<uint8_t> *bytecode);
	void save(UINT64 hash, const wchar_t *shader_type, const std::vector<uint8_t> *bytecode);

	// Writes the metadata out if records were queued since the last flush.
	// Called from the per-frame seam, exactly like the ShaderRegex cache:
	bool flush();

	// Safe to call without the mutex. The per-frame flush uses this to skip the
	// lock entirely when there is nothing to write:
	bool is_dirty() const { return metadata_dirty.load(std::memory_order_relaxed); }

private:
	bool loaded = false;
	std::wstring metadata_path;
	ShaderBytecodeRecordFileHeader header;
	std::vector<ShaderBytecodeRecordEntry> records;
	// Atomic so is_dirty() can be read without the mutex; every mutation still
	// happens under shader_bytecode_record_mutex.
	std::atomic<bool> metadata_dirty{ false };

	BlobStore blob;

	void get_paths(wchar_t *dat_path, wchar_t *blob_path);
	static bool record_less(const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b);

	std::vector<ShaderBytecodeRecordEntry>::iterator lower_bound(ShaderStage stage, uint64_t hash);
	ShaderBytecodeRecordEntry *find_record(ShaderStage stage, uint64_t hash);
	ShaderBytecodeRecordEntry *find_or_insert_record(ShaderStage stage, uint64_t hash);
	bool persist_metadata();
	bool metadata_load(const wchar_t *dat_path);
	bool validate_blob();
	void reset_state();
	void wipe_files();
	bool create_metadata();
	bool ensure_valid(bool create_if_missing);
	void prune_oldest();
};

static std::mutex shader_bytecode_record_mutex;
static ShaderBytecodeRecordStore shader_bytecode_record_store;

void ShaderBytecodeRecordStore::get_paths(wchar_t *dat_path, wchar_t *blob_path)
{
	// blob_path may be NULL when only the metadata (.dat) path is needed:
	dat_path[0] = 0;
	if (blob_path)
		blob_path[0] = 0;

	swprintf_s(dat_path, MAX_PATH, L"%ls\\ShaderBytecodeRecord.dat", G->SHADER_CACHE_PATH);
	if (blob_path)
		swprintf_s(blob_path, MAX_PATH, L"%ls\\ShaderBytecodeRecord.blob", G->SHADER_CACHE_PATH);
}

bool ShaderBytecodeRecordStore::record_less(const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b)
{
	if (a.stage != b.stage)
		return a.stage < b.stage;
	return a.hash < b.hash;
}

std::vector<ShaderBytecodeRecordEntry>::iterator ShaderBytecodeRecordStore::lower_bound(ShaderStage stage, uint64_t hash)
{
	ShaderBytecodeRecordEntry key;
	memset(&key, 0, sizeof(key));
	key.stage = stage;
	key.hash = hash;

	return std::lower_bound(records.begin(), records.end(), key, record_less);
}

ShaderBytecodeRecordEntry *ShaderBytecodeRecordStore::find_record(ShaderStage stage, uint64_t hash)
{
	std::vector<ShaderBytecodeRecordEntry>::iterator i = lower_bound(stage, hash);

	if (i == records.end() || i->stage != stage || i->hash != hash)
		return NULL;
	return &*i;
}

ShaderBytecodeRecordEntry *ShaderBytecodeRecordStore::find_or_insert_record(ShaderStage stage, uint64_t hash)
{
	std::vector<ShaderBytecodeRecordEntry>::iterator i = lower_bound(stage, hash);

	if (i != records.end() && i->stage == stage && i->hash == hash)
		return &*i;

	ShaderBytecodeRecordEntry record;
	memset(&record, 0, sizeof(record));
	record.hash = hash;
	record.stage = stage;

	return &*records.insert(i, record);
}

bool ShaderBytecodeRecordStore::persist_metadata()
{
	const wchar_t *dat_path = metadata_path.c_str();
	std::vector<uint8_t> out;

	header.record_count = (uint32_t)records.size();

	// The metadata is persisted atomically by shader_store_metadata_write().
	// The blob is always appended to *before* this runs, so the persisted
	// metadata never references bytecode that isn't on disk yet:
	size_t size = sizeof(ShaderBytecodeRecordFileHeader)
			+ records.size() * sizeof(ShaderBytecodeRecordEntry);

	out.resize(size);

	memcpy(out.data(), &header, sizeof(ShaderBytecodeRecordFileHeader));
	if (!records.empty())
		memcpy(out.data() + sizeof(ShaderBytecodeRecordFileHeader),
				records.data(), records.size() * sizeof(ShaderBytecodeRecordEntry));

	if (!shader_store_metadata_write(dat_path, out.data(), out.size())) {
		LogWarning("ShaderBytecodeRecord: persist_metadata write to %S FAILED (records=%u size=%Iu)\n",
				dat_path, header.record_count, size);
		return false;
	}
	metadata_dirty = false;
	return true;
}

// Trust boundary for a file that may be truncated, corrupt or tampered with.
// Everything is parsed and validated into locals first and only adopted once
// every record passes, so a false return leaves this store exactly as it was -
// the caller does not need a reset_state() to make a failed load safe.
bool ShaderBytecodeRecordStore::metadata_load(const wchar_t *dat_path)
{
	std::vector<uint8_t> buf;

	if (!shader_store_metadata_read(dat_path, &buf))
		return false;

	if (buf.size() < sizeof(ShaderBytecodeRecordFileHeader))
		return false;

	ShaderBytecodeRecordFileHeader file_header;
	memcpy(&file_header, buf.data(), sizeof(file_header));

	if (file_header.magic != SHADER_BYTECODE_RECORD_CACHE_MAGIC
	 || file_header.version != SHADER_BYTECODE_RECORD_CACHE_VERSION)
		return false;

	size_t expected = sizeof(ShaderBytecodeRecordFileHeader)
			+ (size_t)file_header.record_count * sizeof(ShaderBytecodeRecordEntry);
	if (buf.size() != expected)
		return false;

	// Everything is parsed and validated into locals first, and only adopted
	// once every record passes, so a rejected file cannot leave the store
	// holding a mix of old and new state:
	std::vector<ShaderBytecodeRecordEntry> loaded(file_header.record_count);
	if (!loaded.empty()) {
		memcpy(loaded.data(),
				buf.data() + sizeof(ShaderBytecodeRecordFileHeader),
				loaded.size() * sizeof(ShaderBytecodeRecordEntry));
	}

	// Validate every record: the (stage, hash) sort invariant the binary search
	// relies on, plus the blob ranges:
	ShaderBytecodeRecordEntry prev;
	for (size_t i = 0; i < loaded.size(); i++) {
		ShaderBytecodeRecordEntry &r = loaded[i];

		if (i && !record_less(prev, r))
			return false;
		if (r.stage >= ShaderStage::COUNT)
			return false;
		// blob_size == 0 is tolerated: an old session that failed mid-append
		// can leave such a record, and save() repairs it with a fresh append.
		// Only a claimed block has to be aligned and inside the blob.
		if (r.blob_size
		 && (r.blob_offset % BlobStore::block_alignment()
		 || !shader_store_range_valid(r.blob_offset, r.blob_size, file_header.blob_size)))
			return false;

		prev = r;
	}

	file_header.reserved = 0;
	for (const auto &r : loaded)
		file_header.next_epoch = (std::max)(file_header.next_epoch, r.last_seen);
	this->header = file_header;
	records.swap(loaded);
	return true;
}

// Make sure the blob is at least as large as the metadata claims:
bool ShaderBytecodeRecordStore::validate_blob()
{
	return blob.validate(header.blob_size);
}

void ShaderBytecodeRecordStore::reset_state()
{
	blob.close();

	records.clear();
	loaded = false;
	metadata_dirty = false;
}

void ShaderBytecodeRecordStore::wipe_files()
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	get_paths(dat_path, blob_path);

	reset_state();

	shader_store_delete(dat_path);
	shader_store_delete(blob_path);
}

bool ShaderBytecodeRecordStore::create_metadata()
{
	wchar_t dat_path[MAX_PATH];

	get_paths(dat_path, NULL);

	memset(&header, 0, sizeof(ShaderBytecodeRecordFileHeader));
	header.magic = SHADER_BYTECODE_RECORD_CACHE_MAGIC;
	header.version = SHADER_BYTECODE_RECORD_CACHE_VERSION;
	header.next_epoch = 1;
	loaded = true;
	metadata_dirty = true;

	if (!persist_metadata()) {
		LogWarning("ShaderBytecodeRecord: create_metadata failed to persist %S\n", dat_path);
		reset_state();
		return false;
	}

	LogWarning("ShaderBytecodeRecord: cache created at %S\n", dat_path);
	return true;
}

bool ShaderBytecodeRecordStore::ensure_valid(bool create_if_missing)
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	get_paths(dat_path, blob_path);
	if (blob.set_path(blob_path))
		reset_state();
	metadata_path = dat_path;

	if (loaded) {
		blob.set_logical_end(header.blob_size);
		return true;
	}

	if (metadata_load(dat_path) && validate_blob()) {
		blob.set_logical_end(header.blob_size);
		loaded = true;

		// Don't let the ledger grow unbounded across generations. Prune the
		// oldest records (the blob stays grow-only; dead blocks are skipped
		// over by the remaining record offsets):
		prune_oldest();

		LogWarning("ShaderBytecodeRecord: loaded %u records from %S (blob %Iu)\n",
				(unsigned)records.size(), dat_path, (uint64_t)header.blob_size);
		return true;
	}

	reset_state();

	if (!create_if_missing)
		return false;

	LogWarning("ShaderBytecodeRecord: %S not present or invalid, (re)creating\n", dat_path);
	wipe_files();

	return create_metadata();
}

// Trims to SHADER_BYTECODE_RECORD_MAX records, keeping the most recent. Runs
// after a metadata load and whenever a save pushes the count over the cap.
// Records are always inserted in (stage, hash) sort order but last_seen is not
// sorted, so the trim needs a full re-sort on both counts:
void ShaderBytecodeRecordStore::prune_oldest()
{
	if (records.size() <= SHADER_BYTECODE_RECORD_MAX)
		return;
	// Normal steady-state insertion exceeds the cap by one. Preserve key order
	// with one linear scan/erase instead of sorting the entire ledger twice.
	if (records.size() == SHADER_BYTECODE_RECORD_MAX + 1) {
		auto oldest = std::min_element(records.begin(), records.end(),
				[](const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b) {
					return a.last_seen < b.last_seen;
				});
		records.erase(oldest);
		metadata_dirty = true;
		return;
	}

	std::sort(records.begin(), records.end(),
			[](const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b) {
				return a.last_seen > b.last_seen;
			});

	records.resize(SHADER_BYTECODE_RECORD_MAX);

	// Records must be back in (stage, hash) order for lower_bound() to work:
	std::sort(records.begin(), records.end(), record_less);

	metadata_dirty = true;
}

bool ShaderBytecodeRecordStore::load(UINT64 hash, const wchar_t *shader_type, std::vector<uint8_t> *bytecode)
{
	ShaderBytecodeRecordEntry *record;

	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS)
		return false;

	ShaderStage stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return false;

	if (!ensure_valid(false))
		return false;

	record = find_record(stage, hash);
	if (!record || !record->blob_size)
		return false;

	bytecode->resize(record->blob_size);
	uint32_t version;
	return blob.read(record->blob_offset, record->blob_size, bytecode->data())
			&& shader_store_dxbc_version(bytecode->data(), bytecode->size(), &version)
			&& shader_stage_from_dxbc_type(version >> 16) == stage;
}

void ShaderBytecodeRecordStore::save(UINT64 hash, const wchar_t *shader_type, const std::vector<uint8_t> *bytecode)
{
	ShaderBytecodeRecordEntry *record;

	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS)
		return;
	if (!bytecode || bytecode->empty() || bytecode->size() > UINT32_MAX)
		return;

	ShaderStage stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return;

	if (!ensure_valid(true)) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x aborted - cache not usable\n", shader_type, hash);
		return;
	}

	// Keep recency ordering meaningful even for long-lived ledgers at rollover.
	if (header.next_epoch == UINT32_MAX) {
		std::sort(records.begin(), records.end(),
				[](const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b) {
					return a.last_seen < b.last_seen;
				});
		header.next_epoch = 0;
		for (auto &r : records)
			r.last_seen = ++header.next_epoch;
		std::sort(records.begin(), records.end(), record_less);
	}
	record = find_record(stage, hash);
	if (record && record->blob_size) {
		// Already in the ledger - just refresh its recency and access count
		// (the bytecode is identical so no new blob block is needed):
		record->last_seen = ++header.next_epoch;
		if (record->access_count != UINT32_MAX)
			record->access_count++;
		metadata_dirty = true;
		return;
	}

	// Validate before appending: load() refuses anything this rejects, so
	// persisting it would only strand a dead block. An unledgered shader just
	// costs a re-analysis next session; a poisoned ledger costs every session.
	uint32_t dxbc_version;
	if (!shader_store_dxbc_version(bytecode->data(), bytecode->size(), &dxbc_version)
	 || shader_stage_from_dxbc_type(dxbc_version >> 16) != stage) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x skipped - not valid %S bytecode\n",
				shader_type, hash, shader_type);
		return;
	}

	uint64_t blob_offset;
	if (!blob.append(bytecode->data(), bytecode->size(), &blob_offset)) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x blob append failed (%Iu bytes)\n",
				shader_type, hash, bytecode->size());
		return;
	}

	// Publish only after the append succeeds; failed writes must be retryable.
	record = find_or_insert_record(stage, hash);
	record->blob_offset = blob_offset;
	record->blob_size = (uint32_t)bytecode->size();
	record->last_seen = ++header.next_epoch;
	record->access_count = 1;
	record->flags = 0;
	header.blob_size = blob.logical_end();

	// Must come after the last use of record: prune_oldest() re-sorts the
	// vector, which invalidates the pointer. The cap is also enforced after a
	// load, but a single long session can cross it without ever reloading, and
	// this ledger is generation-independent so nothing else would trim it.
	if (records.size() > SHADER_BYTECODE_RECORD_MAX)
		prune_oldest();

	LogWarning("ShaderBytecodeRecord: saved %S %016I64x at blob offset %Iu (%u bytes)\n",
			shader_type, hash, (uint64_t)blob_offset, (unsigned)bytecode->size());

	// Defer the disk write until the per-frame flush, like the regex cache:
	metadata_dirty = true;
}

bool ShaderBytecodeRecordStore::flush()
{
	if (!metadata_dirty)
		return true;
	return persist_metadata();
}

// ---- Public API ----

void shader_bytecode_record_save(UINT64 hash, const wchar_t *shader_type, const std::vector<uint8_t> *bytecode)
{
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	shader_bytecode_record_store.save(hash, shader_type, bytecode);
}

bool shader_bytecode_record_load(UINT64 hash, const wchar_t *shader_type, std::vector<uint8_t> *bytecode)
{
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.load(hash, shader_type, bytecode);
}

bool shader_bytecode_record_flush()
{
	// Checked before taking the mutex, as in shader_regex_cache_flush(): a
	// no-op flush should never block behind another thread's file I/O.
	if (!shader_bytecode_record_store.is_dirty())
		return true;

	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.flush();
}