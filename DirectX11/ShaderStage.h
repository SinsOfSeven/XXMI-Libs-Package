#pragma once

#include <cstdint>
#include <cstddef>
#include <cwchar>

// Shared enumeration of the D3D11 shader stages, used as the bounded,
// iterable key space for per-stage cache data (records, per-model regex
// hashes, etc). The underlying type is kept to a single byte so it can be
// stored in on-disk records without changing their layout.
enum class ShaderStage : uint8_t {
	VS,
	HS,
	DS,
	GS,
	PS,
	CS,
	COUNT,            // number of valid stages; keep before INVALID
	INVALID = 0xff,
};

struct ShaderStageInfo {
	const wchar_t *label;        // "vs", "hs", ... (cache types, overrides)
	wchar_t letter;              // 'v', 'h', ... ([CustomShader] compile keys)
	const char *model_prefix;    // shader model string prefix (ps_5_0, ...)
	uint32_t dxil_type;          // DXBC SHEX/SHDR version token type field
};

// Order must match ShaderStage exactly so the table can be indexed by stage.
// static constexpr (not inline) because the project compiles C++14; each TU
// gets its own copy of this tiny compile-time table:
static constexpr ShaderStageInfo shader_stage_info[] = {
	{ L"vs", 'v', "vs", 1 },
	{ L"hs", 'h', "hs", 3 },
	{ L"ds", 'd', "ds", 4 },
	{ L"gs", 'g', "gs", 2 },
	{ L"ps", 'p', "ps", 0 },
	{ L"cs", 'c', "cs", 5 },
};

static_assert(sizeof(shader_stage_info) / sizeof(shader_stage_info[0]) == static_cast<size_t>(ShaderStage::COUNT),
		"shader_stage_info must be indexed by ShaderStage");

// Fallback row for ShaderStage::INVALID so callers can ask for metadata about
// an unknown stage without doing their own bounds checking:
inline const ShaderStageInfo &shader_stage_info_at(ShaderStage stage)
{
	static const ShaderStageInfo invalid = { L"xx", 0, "xx", 0xffffffff };
	size_t i = static_cast<size_t>(stage);
	return (i < static_cast<size_t>(ShaderStage::COUNT)) ? shader_stage_info[i] : invalid;
}

inline ShaderStage shader_stage_from_label(const wchar_t *label)
{
	if (label) {
		for (size_t i = 0; i < static_cast<size_t>(ShaderStage::COUNT); i++)
			if (!_wcsicmp(label, shader_stage_info[i].label))
				return static_cast<ShaderStage>(i);
	}
	return ShaderStage::INVALID;
}

inline ShaderStage shader_stage_from_letter(wchar_t letter)
{
	for (size_t i = 0; i < static_cast<size_t>(ShaderStage::COUNT); i++)
		if (shader_stage_info[i].letter == letter)
			return static_cast<ShaderStage>(i);
	return ShaderStage::INVALID;
}

inline ShaderStage shader_stage_from_dxil_type(uint32_t type)
{
	for (size_t i = 0; i < static_cast<size_t>(ShaderStage::COUNT); i++)
		if (shader_stage_info[i].dxil_type == type)
			return static_cast<ShaderStage>(i);
	return ShaderStage::INVALID;
}