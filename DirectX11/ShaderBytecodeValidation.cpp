#include "ShaderBytecodeValidation.h"
#include "BlobStore.h"

#include <cstring>
#include "d3d11TokenizedProgramFormat.hpp"

bool shader_bytecode_dxbc_version(const void *data, size_t size, uint32_t *version)
{
	if (!data || size < 32 || size > UINT32_MAX || memcmp(data, "DXBC", 4))
		return false;
	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t total, count;
	memcpy(&total, bytes + 24, 4);
	memcpy(&count, bytes + 28, 4);
	if (total != size || !count || count > (size - 32) / 4)
		return false;
	bool found = false;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t offset, length;
		memcpy(&offset, bytes + 32 + i * 4, 4);
		if (offset < 32 + (uint64_t)count * 4 || !blob_store_range_valid(offset, 8, size))
			return false;
		memcpy(&length, bytes + offset + 4, 4);
		if (!blob_store_range_valid((uint64_t)offset + 8, length, size))
			return false;
		if (!memcmp(bytes + offset, "SHEX", 4) || !memcmp(bytes + offset, "SHDR", 4)) {
			if (length < 8 || length % 4)
				return false;
			uint32_t words;
			memcpy(version, bytes + offset + 8, 4);
			memcpy(&words, bytes + offset + 12, 4);
			if (words != length / 4 || (*version >> 16) > 5)
				return false;
			found = true;
		}
	}
	return found;
}

bool shader_bytecode_dxbc_instructions(const void *data, size_t size)
{
	uint32_t version;
	if (!shader_bytecode_dxbc_version(data, size, &version))
		return false;
	const uint32_t major = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version);
	const uint32_t minor = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version);
	if (!((major == 4 && minor <= 1) || (major == 5 && minor <= 1)))
		return false;

	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	uint32_t chunk_count;
	memcpy(&chunk_count, bytes + 28, sizeof(chunk_count));
	bool found_code = false;
	for (uint32_t i = 0; i < chunk_count; ++i) {
		uint32_t offset;
		memcpy(&offset, bytes + 32 + i * 4, sizeof(offset));
		const uint8_t *chunk = bytes + offset;
		if (memcmp(chunk, "SHEX", 4) && memcmp(chunk, "SHDR", 4))
			continue;
		if (found_code)
			return false;
		found_code = true;

		uint32_t chunk_size, token_count;
		memcpy(&chunk_size, chunk + 4, sizeof(chunk_size));
		memcpy(&token_count, chunk + 12, sizeof(token_count));
		if (chunk_size < 8 || (chunk_size & 3) || token_count != chunk_size / 4 || token_count < 3)
			return false;
		const uint8_t *tokens = chunk + 16;
		uint32_t remaining = token_count - 2;
		while (remaining) {
			uint32_t token;
			memcpy(&token, tokens, sizeof(token));
			const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(token);
			if (opcode >= D3D10_SB_NUM_OPCODES ||
					opcode == D3D10_SB_OPCODE_RESERVED0 ||
					opcode == D3D10_1_SB_OPCODE_RESERVED1 ||
					opcode == D3D11_SB_OPCODE_RESERVED0 ||
					opcode == D3DWDDM1_3_SB_OPCODE_RESERVED0)
				return false;

			uint32_t length;
			if (opcode == D3D10_SB_OPCODE_CUSTOMDATA) {
				if (remaining < 2)
					return false;
				memcpy(&length, tokens + 4, sizeof(length));
				if (length < 2 || length > remaining)
					return false;
			} else {
				length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
				if (!length || length > remaining)
					return false;
				// Bit 31 chains extended opcode tokens. Their count is part of
				// the instruction length, and must never run into the next one.
				uint32_t ext = token;
				uint32_t header_words = 1;
				while (ext & 0x80000000u) {
					if (header_words >= length || header_words > D3D11_SB_MAX_SIMULTANEOUS_EXTENDED_OPCODES)
						return false;
					memcpy(&ext, tokens + header_words * 4, sizeof(ext));
					++header_words;
				}
			}
			tokens += length * 4;
			remaining -= length;
		}
	}
	return found_code;
}

