#include "ShaderBytecodeRecord.h"

#include <algorithm>
#include <mutex>

#include "globals.h"
#include "log.h"

#define SHADER_BYTECODE_RECORD_CACHE_MAGIC ('S' | ('B' << 8) | ('R' << 16) | ('C' << 24))
#define SHADER_BYTECODE_RECORD_CACHE_VERSION 1

// Cap on the number of records kept. The ledger is generation-independent so
// it grows across ShaderRegex reloads; when the cap is exceeded the oldest
// (by last_seen) records are pruned. Their blob blocks stay behind in the
// grow-only blob file (dead blocks are skipped over by record offsets):
static constexpr size_t SHADER_BYTECODE_RECORD_MAX = 16384;

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

private:
	bool loaded = false;
	ShaderBytecodeRecordFileHeader header;
	std::vector<ShaderBytecodeRecordEntry> records;
	bool metadata_dirty = false;

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

	if (i == records.end()
	 || i->stage != stage || i->hash != hash)
		return NULL;
	return &*i;
}

ShaderBytecodeRecordEntry *ShaderBytecodeRecordStore::find_or_insert_record(ShaderStage stage, uint64_t hash)
{
	std::vector<ShaderBytecodeRecordEntry>::iterator i = lower_bound(stage, hash);

	if (i != records.end()
	 && i->stage == stage && i->hash == hash)
		return &*i;

	ShaderBytecodeRecordEntry record;
	memset(&record, 0, sizeof(record));
	record.hash = hash;
	record.stage = stage;

	return &*records.insert(i, record);
}

bool ShaderBytecodeRecordStore::persist_metadata()
{
	wchar_t dat_path[MAX_PATH];
	std::vector<uint8_t> out;

	metadata_dirty = false;

	get_paths(dat_path, NULL);

	header.record_count = (uint32_t)records.size();

	// The metadata is persisted atomically by shader_store_metadata_write().
	// The blob is always appended to *before* this runs, so the persisted
	// metadata never references bytecode that isn't on disk yet:
	size_t size = sizeof(ShaderBytecodeRecordFileHeader)
			+ records.size() * sizeof(ShaderBytecodeRecordEntry);

	out.resize(size);

	memcpy(out.data(), &header, sizeof(ShaderBytecodeRecordFileHeader));
	memcpy(out.data() + sizeof(ShaderBytecodeRecordFileHeader),
			records.data(),
			records.size() * sizeof(ShaderBytecodeRecordEntry));

	if (!shader_store_metadata_write(dat_path, out.data(), out.size())) {
		LogWarning("ShaderBytecodeRecord: persist_metadata write to %S FAILED (records=%u size=%Iu)\n",
				dat_path, header.record_count, size);
		return false;
	}
	return true;
}

bool ShaderBytecodeRecordStore::metadata_load(const wchar_t *dat_path)
{
	std::vector<uint8_t> buf;

	if (!shader_store_metadata_read(dat_path, &buf))
		return false;

	bool ok = false;

	if (buf.size() >= sizeof(ShaderBytecodeRecordFileHeader)) {
		ShaderBytecodeRecordFileHeader header;
		memcpy(&header, buf.data(), sizeof(header));

		if (header.magic == SHADER_BYTECODE_RECORD_CACHE_MAGIC
		 && header.version == SHADER_BYTECODE_RECORD_CACHE_VERSION) {
			size_t expected = sizeof(ShaderBytecodeRecordFileHeader)
					+ (size_t)header.record_count * sizeof(ShaderBytecodeRecordEntry);

			if (buf.size() == expected) {
				this->header = header;
				records.resize(header.record_count);

				memcpy(records.data(),
						buf.data() + sizeof(ShaderBytecodeRecordFileHeader),
						records.size() * sizeof(ShaderBytecodeRecordEntry));

				// Validate every record: the (stage, hash) sort invariant the
				// binary search relies on, plus the blob ranges:
				ok = true;
				ShaderBytecodeRecordEntry prev;
				for (size_t i = 0; i < records.size() && ok; i++) {
					ShaderBytecodeRecordEntry &r = records[i];

					if (i && !record_less(prev, r))
						ok = false;
					if (r.stage >= ShaderStage::COUNT)
						ok = false;
					if (r.blob_size
					 && r.blob_offset + r.blob_size > (uint64_t)header.blob_size)
						ok = false;

					prev = r;
				}
			}
		}
	}

	return ok;
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

	DeleteFile(dat_path);
	DeleteFile(blob_path);
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
	blob.set_path(blob_path);

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
// only after a metadata load (records are inserted in (stage, hash) sort order
// but last_seen is not sorted, so this needs a full re-sort):
void ShaderBytecodeRecordStore::prune_oldest()
{
	if (records.size() <= SHADER_BYTECODE_RECORD_MAX)
		return;

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
	return blob.read(record->blob_offset, record->blob_size, bytecode->data());
}

void ShaderBytecodeRecordStore::save(UINT64 hash, const wchar_t *shader_type, const std::vector<uint8_t> *bytecode)
{
	ShaderBytecodeRecordEntry *record;

	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS)
		return;
	if (!bytecode || bytecode->empty())
		return;

	ShaderStage stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return;

	if (!ensure_valid(true)) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x aborted - cache not usable\n", shader_type, hash);
		return;
	}

	record = find_record(stage, hash);
	if (record) {
		// Already in the ledger - just refresh its recency and access count
		// (the bytecode is identical so no new blob block is needed):
		record->last_seen = (uint32_t)++header.next_epoch;
		record->access_count++;
		metadata_dirty = true;
		return;
	}

	record = find_or_insert_record(stage, hash);
	if (!record) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x failed to insert record\n", shader_type, hash);
		return;
	}

	uint64_t blob_offset;
	if (!blob.append(bytecode->data(), bytecode->size(), &blob_offset)) {
		LogWarning("ShaderBytecodeRecord: save %S %016I64x blob append failed (%Iu bytes)\n",
				shader_type, hash, bytecode->size());
		return;
	}

	record->blob_offset = blob_offset;
	record->blob_size = (uint32_t)bytecode->size();
	record->last_seen = (uint32_t)++header.next_epoch;
	record->access_count = 1;
	record->flags = 0;
	header.blob_size = blob.logical_end();

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
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.flush();
}