#pragma once

#include <cstdint>
#include <cstddef>

// Structural DXBC validation before disassembly or driver use. Returns the code
// chunk version token; does not validate shader instructions.
bool shader_bytecode_dxbc_version(const void *data, size_t size, uint32_t *version);

// Validate instruction lengths, opcodes, and extended instruction headers.
bool shader_bytecode_dxbc_instructions(const void *data, size_t size);
