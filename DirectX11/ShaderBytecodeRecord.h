#pragma once

#include <cstdint>
#include <vector>

#include "ShaderStage.h"
#include "ShaderStore.h"

// Fixed-size on-disk record for original (unpatched) shader bytecode
// preserved in ShaderBytecodeRecord.blob. Records are keyed (stage, hash)
// and kept sorted for binary search:
struct ShaderBytecodeRecordEntry {
	uint64_t hash;
	uint64_t blob_offset;
	uint32_t blob_size;
	uint32_t last_seen;     // monotonic epoch counter; higher = more recent
	uint32_t access_count;
	uint16_t flags;         // future: DERIVED_DATA_PRESENT etc.
	ShaderStage stage;
	uint8_t _pad;
};

static_assert(sizeof(ShaderBytecodeRecordEntry) == 32,
		"ShaderBytecodeRecordEntry must be 32 bytes");

struct ShaderBytecodeRecordFileHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t record_count;
	uint32_t flags;
	uint64_t blob_size;
	uint64_t next_epoch;    // bumped on each save; last_seen stores epoch at write time
};

// Public API — all functions are thread-safe (internal mutex):
void shader_bytecode_record_save(UINT64 hash, const wchar_t *shader_type,
		const std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_load(UINT64 hash, const wchar_t *shader_type,
		std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_flush();