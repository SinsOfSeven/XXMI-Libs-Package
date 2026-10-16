#include "Filter.h"

#include "log.h"
#include "overlay.h"
#include "globals.h"

// Definitions for the matching core declared in Filter.h. Keeping the
// FuzzyMatch::matches() template here (rather than in the header) avoids
// pulling it into every translation unit; the concrete descriptor types used
// across the code base are instantiated explicitly at the bottom of this file.

FuzzyMatch::FuzzyMatch()
{
	op = FuzzyMatchOp::ALWAYS;
	rhs_type1 = FuzzyMatchOperandType::VALUE;
	rhs_type2 = FuzzyMatchOperandType::VALUE;
	val = 0;
	mask = 0xffffffff;
	numerator = 1;
	denominator = 1;
}

static UINT get_resource_width(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_width(const D3D11_TEXTURE1D_DESC *desc) { return desc->Width; }
static UINT get_resource_width(const D3D11_TEXTURE2D_DESC *desc) { return desc->Width; }
static UINT get_resource_width(const D3D11_TEXTURE3D_DESC *desc) { return desc->Width; }

static UINT get_resource_height(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_height(const D3D11_TEXTURE1D_DESC *desc) { return 0; }
static UINT get_resource_height(const D3D11_TEXTURE2D_DESC *desc) { return desc->Height; }
static UINT get_resource_height(const D3D11_TEXTURE3D_DESC *desc) { return desc->Height; }

static UINT get_resource_depth(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE1D_DESC *desc) { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE2D_DESC *desc) { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE3D_DESC *desc) { return desc->Depth; }

static UINT get_resource_array(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_array(const D3D11_TEXTURE1D_DESC *desc) { return desc->ArraySize; }
static UINT get_resource_array(const D3D11_TEXTURE2D_DESC *desc) { return desc->ArraySize; }
static UINT get_resource_array(const D3D11_TEXTURE3D_DESC *desc) { return 0; }

template <typename DescType>
static UINT eval_field(FuzzyMatchOperandType type, UINT val, const DescType *desc)
{
	switch (type) {
		case FuzzyMatchOperandType::VALUE:
			return val;
		case FuzzyMatchOperandType::WIDTH:
			return get_resource_width(desc);
		case FuzzyMatchOperandType::HEIGHT:
			return get_resource_height(desc);
		case FuzzyMatchOperandType::DEPTH:
			return get_resource_depth(desc);
		case FuzzyMatchOperandType::ARRAY:
			return get_resource_array(desc);
		case FuzzyMatchOperandType::RES_WIDTH:
			return G->mResolutionInfo.width;
		case FuzzyMatchOperandType::RES_HEIGHT:
			return G->mResolutionInfo.height;
	};

	LogOverlay(LOG_DIRE, "BUG: Invalid fuzzy field %u\n", type);

	return val;
}

template <typename DescType>
bool FuzzyMatch::matches(UINT lhs, const DescType *desc) const
{
	UINT effective;

	// Common case:
	if (op == FuzzyMatchOp::ALWAYS)
		return true;

	effective = eval_field(rhs_type1, val, desc);

	// Second named field, for match_byte_width = res_width * res_height in RE7
	effective *= eval_field(rhs_type2, 1, desc);

	return matches_common(lhs, effective);
}

bool FuzzyMatch::matches_uint(UINT lhs) const
{
	// Common case:
	if (op == FuzzyMatchOp::ALWAYS)
		return true;

	if (rhs_type1 != FuzzyMatchOperandType::VALUE)
		return false;

	return matches_common(lhs, val);
}

bool FuzzyMatch::matches_common(UINT lhs, UINT effective) const
{
	// For now just supporting a single integer numerator and denominator,
	// which should be sufficient to match most aspect ratios, downsampled
	// textures and so on. TODO: Add a full expression evaluator.
	if (!denominator)
		return false;
	effective = effective * numerator / denominator;

	switch (op) {
		case FuzzyMatchOp::EQUAL:
			// Only case that the mask applies to, for flags fields
			return ((lhs & mask) == effective);
		case FuzzyMatchOp::LESS:
			return (lhs < effective);
		case FuzzyMatchOp::LESS_EQUAL:
			return (lhs <= effective);
		case FuzzyMatchOp::GREATER:
			return (lhs > effective);
		case FuzzyMatchOp::GREATER_EQUAL:
			return (lhs >= effective);
		case FuzzyMatchOp::NOT_EQUAL:
			return (lhs != effective);
	};

	return false;
}

FilterMatch::FilterMatch() :
	name(),
	matches_buffer(true),
	matches_tex1d(true),
	matches_tex2d(true),
	matches_tex3d(true)
{
}

bool FilterMatch::has_draw_conditions() const
{
	return !FirstVertex.empty() || !FirstIndex.empty() || !FirstInstance.empty()
	    || !VertexCount.empty() || !IndexCount.empty() || !InstanceCount.empty();
}

bool FilterMatch::matches_draw_conditions(const DrawCallInfo *call_info) const
{
	if (!has_draw_conditions())
		return true;

	if (!call_info)
		return false;

	if (!IndexCount.matches_uint(call_info->IndexCount))
		return false;
	if (!FirstIndex.matches_uint(call_info->FirstIndex))
		return false;
	if (!VertexCount.matches_uint(call_info->VertexCount))
		return false;
	if (!FirstVertex.matches_uint(call_info->FirstVertex))
		return false;
	if (!InstanceCount.matches_uint(call_info->InstanceCount))
		return false;
	if (!FirstInstance.matches_uint(call_info->FirstInstance))
		return false;

	return true;
}

bool FilterMatch::matches(const D3D11_BUFFER_DESC *desc, const DrawCallInfo *call_info) const
{
	if (!matches_draw_conditions(call_info))
		return false;
	return matches_desc(desc);
}

bool FilterMatch::matches_desc(const D3D11_BUFFER_DESC *desc) const
{
	if (!matches_buffer)
		return false;

	if (!Usage.matches(desc->Usage, desc))
		return false;
	if (!BindFlags.matches(desc->BindFlags, desc))
		return false;
	if (!CPUAccessFlags.matches(desc->CPUAccessFlags, desc))
		return false;
	if (!MiscFlags.matches(desc->MiscFlags, desc))
		return false;
	if (!ByteWidth.matches(desc->ByteWidth, desc))
		return false;
	if (!StructureByteStride.matches(desc->StructureByteStride, desc))
		return false;
	return true;
}

bool FilterMatch::matches(const D3D11_TEXTURE1D_DESC *desc, const DrawCallInfo *call_info) const
{
	if (!matches_draw_conditions(call_info))
		return false;
	return matches_desc(desc);
}

bool FilterMatch::matches_desc(const D3D11_TEXTURE1D_DESC *desc) const
{
	if (!matches_tex1d)
		return false;

	if (!Usage.matches(desc->Usage, desc))
		return false;
	if (!BindFlags.matches(desc->BindFlags, desc))
		return false;
	if (!CPUAccessFlags.matches(desc->CPUAccessFlags, desc))
		return false;
	if (!MiscFlags.matches(desc->MiscFlags, desc))
		return false;
	if (!MipLevels.matches(desc->MipLevels, desc))
		return false;
	if (!Format.matches(desc->Format, desc))
		return false;
	if (!Width.matches(desc->Width, desc))
		return false;
	if (!ArraySize.matches(desc->ArraySize, desc))
		return false;
	return true;
}

bool FilterMatch::matches(const D3D11_TEXTURE2D_DESC *desc, const DrawCallInfo *call_info) const
{
	if (!matches_draw_conditions(call_info))
		return false;
	return matches_desc(desc);
}

bool FilterMatch::matches_desc(const D3D11_TEXTURE2D_DESC *desc) const
{
	if (!matches_tex2d)
		return false;

	if (!Usage.matches(desc->Usage, desc))
		return false;
	if (!BindFlags.matches(desc->BindFlags, desc))
		return false;
	if (!CPUAccessFlags.matches(desc->CPUAccessFlags, desc))
		return false;
	if (!MiscFlags.matches(desc->MiscFlags, desc))
		return false;
	if (!MipLevels.matches(desc->MipLevels, desc))
		return false;
	if (!Format.matches(desc->Format, desc))
		return false;
	if (!Width.matches(desc->Width, desc))
		return false;
	if (!Height.matches(desc->Height, desc))
		return false;
	if (!ArraySize.matches(desc->ArraySize, desc))
		return false;
	if (!SampleDesc_Count.matches(desc->SampleDesc.Count, desc))
		return false;
	if (!SampleDesc_Quality.matches(desc->SampleDesc.Quality, desc))
		return false;
	return true;
}

bool FilterMatch::matches(const D3D11_TEXTURE3D_DESC *desc, const DrawCallInfo *call_info) const
{
	if (!matches_draw_conditions(call_info))
		return false;
	return matches_desc(desc);
}

bool FilterMatch::matches_desc(const D3D11_TEXTURE3D_DESC *desc) const
{
	if (!matches_tex3d)
		return false;

	if (!Usage.matches(desc->Usage, desc))
		return false;
	if (!BindFlags.matches(desc->BindFlags, desc))
		return false;
	if (!CPUAccessFlags.matches(desc->CPUAccessFlags, desc))
		return false;
	if (!MiscFlags.matches(desc->MiscFlags, desc))
		return false;
	if (!MipLevels.matches(desc->MipLevels, desc))
		return false;
	if (!Format.matches(desc->Format, desc))
		return false;
	if (!Width.matches(desc->Width, desc))
		return false;
	if (!Height.matches(desc->Height, desc))
		return false;
	if (!Depth.matches(desc->Depth, desc))
		return false;
	return true;
}

// Custom resources are matched on their *declared* (override_*) fields, so a
// filter can discriminate between resources before they are ever substantiated.
// The declared fields are synthesised into a D3D11_DESC and matched through the
// regular overloads above, which keeps the matching semantics identical for
// declared and live resources.
bool FilterMatch::matches(const CustomResource *resource, const DrawCallInfo *call_info) const
{
	bool buffer;
	bool tex1d;
	bool tex2d;
	bool tex3d;

	switch (resource->override_type) {
		case CustomResourceType::BUFFER:
		case CustomResourceType::STRUCTURED_BUFFER:
		case CustomResourceType::RAW_BUFFER:
			buffer = true; tex1d = tex2d = tex3d = false;
			break;
		case CustomResourceType::TEXTURE1D:
			buffer = tex2d = tex3d = false; tex1d = true;
			break;
		case CustomResourceType::TEXTURE2D:
		case CustomResourceType::CUBE:
			buffer = tex1d = tex3d = false; tex2d = true;
			break;
		case CustomResourceType::TEXTURE3D:
			buffer = tex1d = tex2d = false; tex3d = true;
			break;
		default:
			// No type declared: try every dimension the filter can match.
			buffer = matches_buffer;
			tex1d = matches_tex1d;
			tex2d = matches_tex2d;
			tex3d = matches_tex3d;
			break;
	}

	if (buffer) {
		D3D11_BUFFER_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (D3D11_BIND_FLAG)resource->override_bind_flags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = (D3D11_RESOURCE_MISC_FLAG)resource->override_misc_flags;
		desc.ByteWidth = resource->override_byte_width;
		desc.StructureByteStride = resource->override_stride;
		if (matches(&desc, call_info))
			return true;
	}
	if (tex1d) {
		D3D11_TEXTURE1D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Format = resource->override_format;
		desc.Width = resource->override_width;
		desc.MipLevels = resource->override_mips;
		desc.ArraySize = resource->override_array;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (D3D11_BIND_FLAG)resource->override_bind_flags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = (D3D11_RESOURCE_MISC_FLAG)resource->override_misc_flags;
		if (matches(&desc, call_info))
			return true;
	}
	if (tex2d) {
		D3D11_TEXTURE2D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Format = resource->override_format;
		desc.Width = resource->override_width;
		desc.Height = resource->override_height;
		desc.MipLevels = resource->override_mips;
		desc.ArraySize = resource->override_array;
		desc.SampleDesc.Count = resource->override_msaa;
		desc.SampleDesc.Quality = resource->override_msaa_quality;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (D3D11_BIND_FLAG)resource->override_bind_flags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = (D3D11_RESOURCE_MISC_FLAG)resource->override_misc_flags;
		if (matches(&desc, call_info))
			return true;
	}
	if (tex3d) {
		D3D11_TEXTURE3D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Format = resource->override_format;
		desc.Width = resource->override_width;
		desc.Height = resource->override_height;
		desc.Depth = resource->override_depth;
		desc.MipLevels = resource->override_mips;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = (D3D11_BIND_FLAG)resource->override_bind_flags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = (D3D11_RESOURCE_MISC_FLAG)resource->override_misc_flags;
		if (matches(&desc, call_info))
			return true;
	}

	return false;
}

void FilterMatch::set_resource_type(D3D11_RESOURCE_DIMENSION type)
{
	switch(type) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			matches_tex1d = matches_tex2d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			matches_buffer = matches_tex2d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			matches_buffer = matches_tex1d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			matches_buffer = matches_tex1d = matches_tex2d = false;
			return;
	}
}

bool FilterMatch::update_types_matched()
{
	// A field that has at least one constraint is non-empty, so a repeated key
	// (e.g. match_height twice) still counts.

	if (!ByteWidth.empty() || !StructureByteStride.empty())
		matches_tex1d = matches_tex2d = matches_tex3d = false;

	if (!MipLevels.empty() || !Format.empty() || !Width.empty())
		matches_buffer = false;

	if (!Height.empty())
		matches_buffer = matches_tex1d = false;

	if (!Depth.empty())
		matches_buffer = matches_tex1d = matches_tex2d = false;

	if (!ArraySize.empty())
		matches_buffer = matches_tex3d = false;

	if (!SampleDesc_Count.empty() || !SampleDesc_Quality.empty())
		matches_buffer = matches_tex1d = matches_tex3d = false;

	return matches_buffer || matches_tex1d || matches_tex2d || matches_tex3d;
}

// Shared recursive evaluation for FilterExpr. Keeping the recursion in a
// template lets the concrete FilterMatch::matches() overloads be resolved by
// the caller's descriptor type.
template <typename DescType>
static bool filter_expr_matches(const FilterExpr &expr, const uint64_t *resource_hash, const DescType *desc, const DrawCallInfo *call_info, const FilterExpr::HashMatcher *hash_matcher)
{
	switch (expr.op) {
		case FilterExprOp::LEAF:
			return expr.leaf && expr.leaf->matches(desc, call_info);
		case FilterExprOp::HASH:
			return hash_matcher ? (*hash_matcher)(expr.hash) : (resource_hash && expr.hash == *resource_hash);
		case FilterExprOp::AND:
			for (const auto &child : expr.children) {
				if (child && !filter_expr_matches(*child, resource_hash, desc, call_info, hash_matcher))
					return false;
			}
			return true;
		case FilterExprOp::OR:
			for (const auto &child : expr.children) {
				if (child && filter_expr_matches(*child, resource_hash, desc, call_info, hash_matcher))
					return true;
			}
			return false;
		case FilterExprOp::XOR: {
			// Odd number of true children, so binary XOR is the base case and
			// the operator stays associative for longer chains.
			bool result = false;
			for (const auto &child : expr.children) {
				if (child && filter_expr_matches(*child, resource_hash, desc, call_info, hash_matcher))
					result = !result;
			}
			return result;
		}
		case FilterExprOp::NOT:
			return expr.children.size() == 1
				&& !filter_expr_matches(*expr.children[0], resource_hash, desc, call_info, hash_matcher);
	}
	return false;
}

bool FilterExpr::matches(const uint64_t *resource_hash, const D3D11_BUFFER_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher) const
{
	return filter_expr_matches(*this, resource_hash, desc, call_info, hash_matcher);
}

bool FilterExpr::matches(const uint64_t *resource_hash, const D3D11_TEXTURE1D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher) const
{
	return filter_expr_matches(*this, resource_hash, desc, call_info, hash_matcher);
}

bool FilterExpr::matches(const uint64_t *resource_hash, const D3D11_TEXTURE2D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher) const
{
	return filter_expr_matches(*this, resource_hash, desc, call_info, hash_matcher);
}

bool FilterExpr::matches(const uint64_t *resource_hash, const D3D11_TEXTURE3D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher) const
{
	return filter_expr_matches(*this, resource_hash, desc, call_info, hash_matcher);
}

bool FilterExpr::matches(const uint64_t *resource_hash, const CustomResource *resource, const DrawCallInfo *call_info, const HashMatcher *hash_matcher) const
{
	return filter_expr_matches(*this, resource_hash, resource, call_info, hash_matcher);
}

// Only the descriptor types actually used by FilterMatch are instantiated, so
// other translation units (e.g. ResourceHash.cpp) can use FuzzyMatch::matches()
// without having the template definition available.
template bool FuzzyMatch::matches<D3D11_BUFFER_DESC>(UINT lhs, const D3D11_BUFFER_DESC *desc) const;
template bool FuzzyMatch::matches<D3D11_TEXTURE1D_DESC>(UINT lhs, const D3D11_TEXTURE1D_DESC *desc) const;
template bool FuzzyMatch::matches<D3D11_TEXTURE2D_DESC>(UINT lhs, const D3D11_TEXTURE2D_DESC *desc) const;
template bool FuzzyMatch::matches<D3D11_TEXTURE3D_DESC>(UINT lhs, const D3D11_TEXTURE3D_DESC *desc) const;
