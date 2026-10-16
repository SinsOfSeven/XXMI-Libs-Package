#include "ShaderBytecodeRecord.h"
#include "ShaderBytecodeValidation.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "ByteCodeReader.h"
#include "Overlay.h"
#include "globals.h"
#include "log.h"

#define SHADER_BYTECODE_RECORD_CACHE_MAGIC ('S' | ('B' << 8) | ('R' << 16) | ('C' << 24))
#define SHADER_BYTECODE_RECORD_CACHE_VERSION 2

#pragma pack(push, 1)
struct ShaderDeclarationBlockHeader {
	uint8_t version, cb_count, srv_count, reserved;
	uint16_t cb_mask;
	uint8_t srv_mask[16];
};
struct ShaderDeclarationCB {
	uint8_t slot, type;
	uint32_t size;
};
struct ShaderDeclarationSRV {
	uint8_t slot, type, dimension, reserved;
	uint32_t stride;
};
#pragma pack(pop)
static_assert(sizeof(ShaderDeclarationBlockHeader) == 22, "Declaration header layout changed");
static_assert(sizeof(ShaderDeclarationCB) == 6, "CB declaration layout changed");
static_assert(sizeof(ShaderDeclarationSRV) == 8, "SRV declaration layout changed");

static uint32_t declaration_offset(const ShaderBytecodeRecordEntry &record)
{
	return (((uint32_t)record._pad << 16) | record.flags) * 4;
}

static void set_declaration_offset(ShaderBytecodeRecordEntry *record, uint32_t offset)
{
	uint32_t units = offset / 4;
	record->flags = (uint16_t)units;
	record->_pad = (uint8_t)(units >> 16);
}

static size_t declaration_block_size(const uint8_t *data, size_t available)
{
	if (available < sizeof(ShaderDeclarationBlockHeader))
		return 0;
	ShaderDeclarationBlockHeader h;
	memcpy(&h, data, sizeof(h));
	return sizeof(h) + h.cb_count * sizeof(ShaderDeclarationCB) + h.srv_count * sizeof(ShaderDeclarationSRV);
}

static bool declaration_block_valid(const uint8_t *data, size_t size)
{
	if (size < sizeof(ShaderDeclarationBlockHeader))
		return false;
	ShaderDeclarationBlockHeader h;
	memcpy(&h, data, sizeof(h));
	if (h.version != 1 || h.reserved || h.cb_count > D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
			h.srv_count > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
			size != sizeof(h) + h.cb_count * sizeof(ShaderDeclarationCB) + h.srv_count * sizeof(ShaderDeclarationSRV) ||
			(h.cb_mask & ~((1u << D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) - 1)))
		return false;
	uint16_t cb_mask = 0;
	uint8_t srv_mask[16] = {};
	size_t pos = sizeof(h);
	for (unsigned i = 0; i < h.cb_count; ++i) {
		ShaderDeclarationCB cb;
		memcpy(&cb, data + pos, sizeof(cb));
		pos += sizeof(cb);
		if (cb.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
				cb.type == 0 || cb.type > 2 || (cb_mask & (1u << cb.slot)))
			return false;
		cb_mask |= (uint16_t)(1u << cb.slot);
	}
	for (unsigned i = 0; i < h.srv_count; ++i) {
		ShaderDeclarationSRV srv;
		memcpy(&srv, data + pos, sizeof(srv));
		pos += sizeof(srv);
		if (srv.slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
				srv.type == 0 || srv.type > 3 || srv.reserved ||
				(srv_mask[srv.slot / 8] & (1u << (srv.slot % 8))))
			return false;
		srv_mask[srv.slot / 8] |= (uint8_t)(1u << (srv.slot % 8));
	}
	return cb_mask == h.cb_mask && !memcmp(srv_mask, h.srv_mask, sizeof(srv_mask));
}

static std::vector<uint8_t> pack_declarations(const ShaderBindings &bindings)
{
	ShaderDeclarationBlockHeader h = {};
	h.version = 1;
	std::vector<ShaderDeclarationCB> cbs;
	std::vector<ShaderDeclarationSRV> srvs;
	for (unsigned slot = 0; slot < bindings.constant_buffers.size(); ++slot) {
		const auto &cb = bindings.constant_buffers[slot];
		if (cb.type == ShaderConstantBufferType::NONE)
			continue;
		h.cb_mask |= (uint16_t)(1u << slot);
		cbs.push_back({(uint8_t)slot, (uint8_t)cb.type, cb.size});
	}
	for (unsigned slot = 0; slot < bindings.resources.size(); ++slot) {
		const auto &srv = bindings.resources[slot];
		if (srv.type == ShaderResourceType::NONE)
			continue;
		h.srv_mask[slot / 8] |= (uint8_t)(1u << (slot % 8));
		srvs.push_back({(uint8_t)slot, (uint8_t)srv.type, (uint8_t)srv.dimension, 0, srv.stride});
	}
	h.cb_count = (uint8_t)cbs.size();
	h.srv_count = (uint8_t)srvs.size();
	std::vector<uint8_t> out(sizeof(h) + cbs.size() * sizeof(ShaderDeclarationCB) + srvs.size() * sizeof(ShaderDeclarationSRV));
	memcpy(out.data(), &h, sizeof(h));
	if (!cbs.empty())
		memcpy(out.data() + sizeof(h), cbs.data(), cbs.size() * sizeof(ShaderDeclarationCB));
	if (!srvs.empty())
		memcpy(out.data() + sizeof(h) + cbs.size() * sizeof(ShaderDeclarationCB), srvs.data(), srvs.size() * sizeof(ShaderDeclarationSRV));
	return out;
}

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
	bool save_declarations(UINT64 hash, const wchar_t *shader_type, const void *bytecode, size_t size);
	bool query_declaration(UINT64 hash, ShaderStage stage, ShaderDeclarationField field, UINT slot, uint32_t *value);
	bool query_declaration_range(UINT64 hash, ShaderStage stage, bool constant_buffer,
		uint32_t *first, uint32_t *last);

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
	std::vector<uint8_t> declaration_arena;
	// Keep declarations for shaders created this session even if the persistent
	// ledger prunes an old record. Blocks remain compact and stage-keyed.
	std::map<std::pair<ShaderStage, uint64_t>, std::vector<uint8_t>> runtime_declarations;
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
	void compact_declarations();
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
	header.declaration_arena_size = (uint32_t)declaration_arena.size();

	// The metadata is persisted atomically by blob_store_metadata_write().
	// The blob is always appended to *before* this runs, so the persisted
	// metadata never references bytecode that isn't on disk yet:
	size_t size = sizeof(ShaderBytecodeRecordFileHeader)
			+ records.size() * sizeof(ShaderBytecodeRecordEntry) + declaration_arena.size();
	if (size > BLOB_STORE_METADATA_MAX)
		return false;

	out.resize(size);

	memcpy(out.data(), &header, sizeof(ShaderBytecodeRecordFileHeader));
	if (!records.empty())
		memcpy(out.data() + sizeof(ShaderBytecodeRecordFileHeader),
				records.data(), records.size() * sizeof(ShaderBytecodeRecordEntry));
	if (!declaration_arena.empty())
		memcpy(out.data() + sizeof(ShaderBytecodeRecordFileHeader)
				+ records.size() * sizeof(ShaderBytecodeRecordEntry),
				declaration_arena.data(), declaration_arena.size());

	if (!blob_store_metadata_write(dat_path, out.data(), out.size())) {
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

	if (!blob_store_metadata_read(dat_path, &buf))
		return false;

	if (buf.size() < sizeof(ShaderBytecodeRecordFileHeader))
		return false;

	ShaderBytecodeRecordFileHeader file_header;
	memcpy(&file_header, buf.data(), sizeof(file_header));

	if (file_header.magic != SHADER_BYTECODE_RECORD_CACHE_MAGIC
	 || file_header.version != SHADER_BYTECODE_RECORD_CACHE_VERSION)
		return false;

	const size_t arena_size = file_header.declaration_arena_size;
	if (arena_size < 4 || file_header.record_count > (buf.size() - sizeof(ShaderBytecodeRecordFileHeader)) / sizeof(ShaderBytecodeRecordEntry) ||
			arena_size > buf.size() - sizeof(ShaderBytecodeRecordFileHeader) - (size_t)file_header.record_count * sizeof(ShaderBytecodeRecordEntry))
		return false;
	size_t expected = sizeof(ShaderBytecodeRecordFileHeader)
			+ (size_t)file_header.record_count * sizeof(ShaderBytecodeRecordEntry) + arena_size;
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
	std::vector<uint8_t> loaded_arena(arena_size);
	if (arena_size)
		memcpy(loaded_arena.data(), buf.data() + sizeof(ShaderBytecodeRecordFileHeader)
				+ loaded.size() * sizeof(ShaderBytecodeRecordEntry), arena_size);

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
		 || !blob_store_range_valid(r.blob_offset, r.blob_size, file_header.blob_size)))
			return false;
		uint32_t decl_offset = declaration_offset(r);
		if (decl_offset) {
			if (decl_offset >= loaded_arena.size())
				return false;
			size_t decl_size = declaration_block_size(loaded_arena.data() + decl_offset,
					loaded_arena.size() - decl_offset);
			if (!decl_size || !blob_store_range_valid(decl_offset, decl_size, loaded_arena.size()) ||
					!declaration_block_valid(loaded_arena.data() + decl_offset, decl_size))
				return false;
		}

		prev = r;
	}

	file_header.declaration_arena_size = (uint32_t)loaded_arena.size();
	for (const auto &r : loaded)
		file_header.next_epoch = (std::max)(file_header.next_epoch, r.last_seen);
	this->header = file_header;
	records.swap(loaded);
	declaration_arena.swap(loaded_arena);
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
	declaration_arena.clear();
	loaded = false;
	metadata_dirty = false;
}

void ShaderBytecodeRecordStore::wipe_files()
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	get_paths(dat_path, blob_path);

	reset_state();

	blob_store_delete(dat_path);
	blob_store_delete(blob_path);
}

bool ShaderBytecodeRecordStore::create_metadata()
{
	wchar_t dat_path[MAX_PATH];

	get_paths(dat_path, NULL);

	memset(&header, 0, sizeof(ShaderBytecodeRecordFileHeader));
	header.magic = SHADER_BYTECODE_RECORD_CACHE_MAGIC;
	header.version = SHADER_BYTECODE_RECORD_CACHE_VERSION;
	header.next_epoch = 1;
	declaration_arena.assign(4, 0); // Offset zero means no declaration block.
	loaded = true;
	metadata_dirty = true;

	if (!persist_metadata()) {
		LogWarning("ShaderBytecodeRecord: create_metadata failed to persist %S\n", dat_path);
		reset_state();
		return false;
	}

	LogInfo("ShaderBytecodeRecord: cache created at %S\n", dat_path);
	return true;
}

bool ShaderBytecodeRecordStore::ensure_valid(bool create_if_missing)
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];
	get_paths(dat_path, blob_path);
	const bool metadata_exists = GetFileAttributesW(dat_path) != INVALID_FILE_ATTRIBUTES;
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

		LogInfo("ShaderBytecodeRecord: loaded %u records from %S (blob %Iu)\n",
				(unsigned)records.size(), dat_path, (uint64_t)header.blob_size);
		return true;
	}

	reset_state();

	if (!create_if_missing)
		return false;

	if (metadata_exists)
		LogWarning("ShaderBytecodeRecord: invalid cache %S, recreating\n", dat_path);
	else
		LogInfo("ShaderBytecodeRecord: cache not present at %S, creating\n", dat_path);
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
		compact_declarations();
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
	compact_declarations();

	metadata_dirty = true;
}

void ShaderBytecodeRecordStore::compact_declarations()
{
	size_t live = 4;
	for (const auto &record : records) {
		uint32_t offset = declaration_offset(record);
		if (!offset)
			continue;
		size_t size = declaration_block_size(declaration_arena.data() + offset,
				declaration_arena.size() - offset);
		live += (size + 3) & ~size_t(3);
	}
	// Avoid copying the arena on every single record pruned at the cap.
	if (declaration_arena.size() <= live + live / 2)
		return;
	std::vector<uint8_t> compact(4, 0);
	compact.reserve(live);
	for (auto &record : records) {
		uint32_t old = declaration_offset(record);
		if (!old)
			continue;
		size_t size = declaration_block_size(declaration_arena.data() + old,
				declaration_arena.size() - old);
		set_declaration_offset(&record, (uint32_t)compact.size());
		compact.insert(compact.end(), declaration_arena.begin() + old,
				declaration_arena.begin() + old + size);
		compact.resize((compact.size() + 3) & ~size_t(3), 0);
	}
	declaration_arena.swap(compact);
}

bool ShaderBytecodeRecordStore::save_declarations(UINT64 hash, const wchar_t *shader_type,
		const void *bytecode, size_t size)
{
	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS)
		return false;
	ShaderStage stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return false;
	if (runtime_declarations.find({stage, hash}) != runtime_declarations.end())
		return true;
	if (!ensure_valid(true))
		return false;
	auto record = find_record(stage, hash);
	if (record && declaration_offset(*record)) {
		uint32_t offset = declaration_offset(*record);
		size_t block_size = declaration_block_size(declaration_arena.data() + offset,
				declaration_arena.size() - offset);
		runtime_declarations[{stage, hash}] = std::vector<uint8_t>(
				declaration_arena.begin() + offset, declaration_arena.begin() + offset + block_size);
		return true;
	}
	ShaderBindings bindings{};
	if (!get_shader_bindings_from_bytecode(bytecode, size, &bindings))
		return false;
	auto block = pack_declarations(bindings);
	size_t aligned_size = (block.size() + 3) & ~size_t(3);
	if (declaration_arena.size() > UINT32_MAX - aligned_size ||
			declaration_arena.size() / 4 >= (1u << 24) ||
			sizeof(header) + (records.size() + 1) * sizeof(ShaderBytecodeRecordEntry)
				+ declaration_arena.size() + aligned_size > BLOB_STORE_METADATA_MAX)
		return false;
	record = find_or_insert_record(stage, hash);
	if (header.next_epoch == UINT32_MAX) {
		std::sort(records.begin(), records.end(),
				[](const ShaderBytecodeRecordEntry &a, const ShaderBytecodeRecordEntry &b) {
					return a.last_seen < b.last_seen;
				});
		header.next_epoch = 0;
		for (auto &r : records)
			r.last_seen = ++header.next_epoch;
		std::sort(records.begin(), records.end(), record_less);
		record = find_record(stage, hash);
	}
	record->last_seen = ++header.next_epoch;
	record->access_count = 1;
	set_declaration_offset(record, (uint32_t)declaration_arena.size());
	declaration_arena.insert(declaration_arena.end(), block.begin(), block.end());
	declaration_arena.resize(declaration_arena.size() + aligned_size - block.size(), 0);
	runtime_declarations[{stage, hash}] = std::move(block);
	metadata_dirty = true;
	if (records.size() > SHADER_BYTECODE_RECORD_MAX)
		prune_oldest();
	return true;
}

bool ShaderBytecodeRecordStore::query_declaration(UINT64 hash, ShaderStage stage,
		ShaderDeclarationField field, UINT slot, uint32_t *value)
{
	if (!value || stage == ShaderStage::INVALID)
		return false;
	const uint8_t *block = nullptr;
	auto runtime = runtime_declarations.find({stage, hash});
	if (runtime != runtime_declarations.end()) {
		block = runtime->second.data();
	} else {
		if (!loaded && (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS || !ensure_valid(false)))
			return false;
		auto record = find_record(stage, hash);
		if (!record || !declaration_offset(*record))
			return false;
		block = declaration_arena.data() + declaration_offset(*record);
	}
	ShaderDeclarationBlockHeader h;
	memcpy(&h, block, sizeof(h));
	if (field == ShaderDeclarationField::CB_MASK) {
		*value = h.cb_mask;
		return true;
	}
	if (field == ShaderDeclarationField::SRV_DECLARES) {
		if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
			return false;
		*value = (h.srv_mask[slot / 8] >> (slot % 8)) & 1u;
		return true;
	}
	const uint8_t *entries = block + sizeof(h);
	if (field == ShaderDeclarationField::CB_TYPE || field == ShaderDeclarationField::CB_SIZE) {
		if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
			return false;
		*value = field == ShaderDeclarationField::CB_TYPE ? 0 : UINT32_MAX;
		if (!(h.cb_mask & (1u << slot)))
			return true;
		for (unsigned i = 0; i < h.cb_count; ++i) {
			ShaderDeclarationCB cb;
			memcpy(&cb, entries + i * sizeof(cb), sizeof(cb));
			if (cb.slot == slot) {
				*value = field == ShaderDeclarationField::CB_TYPE ? cb.type : cb.size;
				return true;
			}
		}
		return false;
	}
	if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
		return false;
	*value = field == ShaderDeclarationField::SRV_TYPE ? 0 : UINT32_MAX;
	if (!(h.srv_mask[slot / 8] & (1u << (slot % 8))))
		return true;
	entries += h.cb_count * sizeof(ShaderDeclarationCB);
	for (unsigned i = 0; i < h.srv_count; ++i) {
		ShaderDeclarationSRV srv;
		memcpy(&srv, entries + i * sizeof(srv), sizeof(srv));
		if (srv.slot == slot) {
			switch (field) {
			case ShaderDeclarationField::SRV_TYPE: *value = srv.type; break;
			case ShaderDeclarationField::SRV_STRIDE: *value = srv.stride; break;
			case ShaderDeclarationField::SRV_DIMENSION: *value = srv.dimension; break;
			default: return false;
			}
			return true;
		}
	}
	return false;
}

bool ShaderBytecodeRecordStore::query_declaration_range(UINT64 hash, ShaderStage stage,
		bool constant_buffer, uint32_t *first, uint32_t *last)
{
	if (!first || !last || stage == ShaderStage::INVALID)
		return false;
	const uint8_t *block = nullptr;
	auto runtime = runtime_declarations.find({stage, hash});
	if (runtime != runtime_declarations.end()) {
		block = runtime->second.data();
	} else {
		if (!loaded && (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS || !ensure_valid(false)))
			return false;
		auto record = find_record(stage, hash);
		if (!record || !declaration_offset(*record))
			return false;
		block = declaration_arena.data() + declaration_offset(*record);
	}
	ShaderDeclarationBlockHeader h;
	memcpy(&h, block, sizeof(h));
	*first = UINT32_MAX;
	*last = UINT32_MAX;
	if (constant_buffer) {
		for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
			if (!(h.cb_mask & (1u << slot)))
				continue;
			if (*first == UINT32_MAX)
				*first = slot;
			*last = slot;
		}
	} else {
		for (uint32_t slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
			if (!(h.srv_mask[slot / 8] & (1u << (slot % 8))))
				continue;
			if (*first == UINT32_MAX)
				*first = slot;
			*last = slot;
		}
	}
	return true;
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
			&& shader_bytecode_dxbc_version(bytecode->data(), bytecode->size(), &version)
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
	if (!shader_bytecode_dxbc_version(bytecode->data(), bytecode->size(), &dxbc_version)
	 || shader_stage_from_dxbc_type(dxbc_version >> 16) != stage) {
		LogOverlayW(LOG_WARNING, L"ShaderBytecodeRecord: save %ls %016I64x skipped - not valid %ls bytecode\n",
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
	header.blob_size = blob.logical_end();

	// Must come after the last use of record: prune_oldest() re-sorts the
	// vector, which invalidates the pointer. The cap is also enforced after a
	// load, but a single long session can cross it without ever reloading, and
	// this ledger is generation-independent so nothing else would trim it.
	if (records.size() > SHADER_BYTECODE_RECORD_MAX)
		prune_oldest();

	LogDebug("ShaderBytecodeRecord: saved %S %016I64x at blob offset %Iu (%u bytes)\n",
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

bool shader_bytecode_record_save_declarations(UINT64 hash, const wchar_t *shader_type,
		const void *bytecode, size_t size)
{
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.save_declarations(hash, shader_type, bytecode, size);
}

bool shader_bytecode_record_query_declaration(UINT64 hash, ShaderStage stage,
		ShaderDeclarationField field, UINT slot, uint32_t *value)
{
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.query_declaration(hash, stage, field, slot, value);
}

bool shader_bytecode_record_query_declaration_range(UINT64 hash, ShaderStage stage,
		bool constant_buffer, uint32_t *first, uint32_t *last)
{
	std::lock_guard<std::mutex> lock(shader_bytecode_record_mutex);
	return shader_bytecode_record_store.query_declaration_range(hash, stage,
			constant_buffer, first, last);
}
