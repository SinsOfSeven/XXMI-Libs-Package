#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "ShaderStage.h"
#include "BlobStore.h"

// The ledger is deliberately NOT generation-keyed, unlike ShaderRegexCache: the
// bytecode it holds is the game's original, unpatched shader, which is valid
// regardless of which [ShaderRegex] sections are configured. So editing a regex
// rebuilds ShaderRegexCache but leaves this pair alone, and the two files do not
// need to be kept in sync or wiped together.
//
// On-disk format below. Records are keyed (stage, hash) and kept sorted for
// binary search. Version 2 stores a 24-bit, 4-byte-aligned declaration arena
// offset in flags + _pad; the separate .blob layout does not change.
struct ShaderBytecodeRecordEntry {
	uint64_t hash;
	uint64_t blob_offset;
	uint32_t blob_size;
	uint32_t last_seen;     // monotonic epoch counter; higher = more recent
	uint32_t access_count;
	uint16_t flags;         // low 16 bits of declaration offset / 4
	ShaderStage stage;
	uint8_t _pad;           // high 8 bits of declaration offset / 4
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
	uint32_t declaration_arena_size; // version 2; reserved in version 1
};

static_assert(sizeof(ShaderBytecodeRecordFileHeader) == 32, "Ledger header layout changed");

// Public API — all functions are thread-safe (internal mutex):
void shader_bytecode_record_save(UINT64 hash, const wchar_t *shader_type,
		const std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_load(UINT64 hash, const wchar_t *shader_type,
		std::vector<uint8_t> *bytecode);
bool shader_bytecode_record_flush();

// Compact, versioned declaration blocks live in the .dat arena. The reader
// reports false for unknown declarations; a known but absent slot reports
// zero for type/predicate fields, or UINT32_MAX for size/stride/dimension.
enum class ShaderDeclarationField : uint8_t {
	CB_TYPE, CB_SIZE, SRV_TYPE, SRV_STRIDE, SRV_DIMENSION,
	CB_MASK, SRV_DECLARES,
};

bool shader_bytecode_record_save_declarations(UINT64 hash, const wchar_t *shader_type,
		const void *bytecode, size_t size);
bool shader_bytecode_record_query_declaration(UINT64 hash, ShaderStage stage,
		ShaderDeclarationField field, UINT slot, uint32_t *value);
bool shader_bytecode_record_query_declaration_range(UINT64 hash, ShaderStage stage,
		bool constant_buffer, uint32_t *first, uint32_t *last);
