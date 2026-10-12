#include "ShaderBytecodeValidation.h"
#include "BlobStore.h"

#include <cstring>

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
