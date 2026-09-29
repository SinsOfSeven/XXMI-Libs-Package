#pragma once

#include <cstdint>
#include <vector>

#include "ShaderStage.h"
#include "ShaderStore.h"

// The ledger is deliberately NOT generation-keyed, unlike ShaderRegexCache: the
// bytecode it holds is the game's original, unpatched shader, which is valid
// regardless of which [ShaderRegex] sections are configured. So editing a regex
// rebuilds ShaderRegexCache but leaves this pair alone, and the two files do not
// need to be kept in sync or wiped together.
//
// On-disk format below. Records are keyed (stage, hash) and kept sorted for
// binary search. The 32-byte stride packs two entries per 64 bytes. This is
// compact storage, not a guarantee of cache-line alignment of std::vector.
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
	// Bumped on each save; entry::last_seen stores the epoch at write time.
	// save() rebases the live epochs before this counter would wrap.
	uint32_t next_epoch;
	uint32_t reserved; // formerly padding / high half of the original uint64 epoch
};

static_assert(sizeof(ShaderBytecodeRecordFileHeader) == 32, "Ledger header layout changed");

// Public API — all functions are thread-safe (internal mutex):
void shader_bytecode_record_save(UINT64 hash, const wchar_t *shader_type,
		const std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_load(UINT64 hash, const wchar_t *shader_type,
		std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_flush();