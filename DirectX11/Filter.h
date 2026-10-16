#pragma once

#include <d3d11_1.h>
#include <stdint.h>
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "DrawCallInfo.h"

// The resource/shader matching core shared by the fuzzy [TextureOverride]
// matcher and [Filter<Name>] filters.
//
// This header is deliberately independent of globals.h and CommandList.h:
// CustomResource is only matched through a forward declaration, and the
// remaining dependencies are the D3D11 descriptor and DrawCallInfo types
// themselves. Keep it that way so callers can include it cheaply.

enum class FuzzyMatchOp {
	ALWAYS,
	EQUAL,
	LESS,
	LESS_EQUAL,
	GREATER,
	GREATER_EQUAL,
	NOT_EQUAL,
};

enum class FuzzyMatchOperandType {
	VALUE,
	WIDTH,      // Width, Height & Depth useful for checking
	HEIGHT,     // for square/cube/rectangular textures.
	DEPTH,
	ARRAY,      // Probably not useful, but similar to depth
	RES_WIDTH,  // Useful for detecting full screen buffers
	RES_HEIGHT, // including arbitrary multiples of the resolution
};

class FuzzyMatch {
	bool matches_common(UINT lhs, UINT effective) const;
public:
	FuzzyMatchOp op;
	FuzzyMatchOperandType rhs_type1;
	FuzzyMatchOperandType rhs_type2;

	// TODO: Support more operand types, such as texture/resolution
	// width/height. Maybe for advanced usage even allow an operand to be
	// an ini param so it can be changed on the fly (might be useful for
	// MEA to replace the mid-game profile switch, but I'd be surprised if
	// there isn't a better way to achieve that).
	UINT val;
	UINT mask;
	UINT numerator;
	UINT denominator;

	FuzzyMatch();
	template <typename DescType>
	bool matches(UINT lhs, const DescType *desc) const;
	bool matches_uint(UINT lhs) const;
};

// A single filterable field on a resource. A FilterField collects every
// constraint written for it (joined with '&'), and every constraint must pass
// for the field to match. This allows lower & upper bounds on the same field
// (e.g. match_height = >100 & <500) in both [Filter<Name>] and fuzzy
// [TextureOverride] sections.
class FilterField {
public:
	bool empty() const { return values.empty(); }
	void add(const FuzzyMatch &m) { values.push_back(m); }
	const std::vector<FuzzyMatch> &constraints() const { return values; }

	// True if the field has exactly one plain `= value` constraint, and stores
	// that value. This is the only form the draw info fast-path maps can index
	// on (see BuildTextureOverrideDrawMaps), so anything else must fall back to
	// scanning the overrides.
	bool single_equal_value(UINT *out) const
	{
		if (values.size() != 1)
			return false;

		const FuzzyMatch &m = values.front();
		if (m.op != FuzzyMatchOp::EQUAL || m.rhs_type1 != FuzzyMatchOperandType::VALUE)
			return false;

		*out = m.val;
		return true;
	}

	// Matches the field value against a typed resource description. NOTE: Only
	// safe to instantiate in translation units where FuzzyMatch::matches is
	// defined (i.e. Filter.cpp), so keep the call sites of these out of other
	// files - use the concrete FilterMatch::matches() overloads instead.
	template <typename DescType>
	bool matches(UINT lhs, const DescType *desc) const
	{
		for (const FuzzyMatch &m : values) {
			if (!m.matches(lhs, desc))
				return false;
		}
		return true;
	}

	// Matches the field value directly (draw context fields, where the RHS of
	// every constraint must be a literal value).
	bool matches_uint(UINT lhs) const
	{
		for (const FuzzyMatch &m : values) {
			if (!m.matches_uint(lhs))
				return false;
		}
		return true;
	}
private:
	std::vector<FuzzyMatch> values;
};

// Forward declaration to resolve the include cycle (CustomResource is defined
// in CommandList.h, which includes ResourceHash.h, which includes this header):
class CustomResource;

// A single [Filter<Name>] section: a set of constraints on resource
// descriptions (and optionally the draw context) that can be evaluated against
// any resource from a command list via the `->Match(<expression>)` member
// (see FilterExpr for how several filters are combined). Unlike a fuzzy
// [TextureOverride], a filter is pure - it never maps
// to a TextureOverride, so evaluating one is cheap, and it can also match the
// *declared* fields of a custom resource ([Resource...] section) rather than
// only live D3D11 resources.
//
// TODO(shader filters): a shader-matched filter will reuse this class, but its
// match context is derived shader data (bytecode reflection / bindings) rather
// than a resource description. Add shader-side fields here when that lands.
class FilterMatch {
public:
	std::wstring name; // Lowercased section id, as registered in G->mFilters

	bool matches_buffer;
	bool matches_tex1d;
	bool matches_tex2d;
	bool matches_tex3d;

	FilterField Usage;               // Common
	FilterField BindFlags;           // Common
	FilterField CPUAccessFlags;      // Common
	FilterField MiscFlags;           // Common
	FilterField ByteWidth;           // Buffer+StructuredBuffer
	FilterField StructureByteStride; // StructuredBuffer
	FilterField MipLevels;           // 1D+2D+3D
	FilterField Format;              // 1D+2D+3D
	FilterField Width;               // 1D+2D+3D
	FilterField Height;              //    2D+3D
	FilterField Depth;               //       3D
	FilterField ArraySize;           // 1D+2D
	FilterField SampleDesc_Count;    //    2D
	FilterField SampleDesc_Quality;  //    2D
	// Creation argument state used by fuzzy [TextureOverride] matching. This is
	// not part of a D3D11 resource description and is unavailable to [Filter].
	bool initial_data_match_set = false;
	bool initial_data_expected = false;

	// Draw context fields, evaluated against the DrawCallInfo of the current
	// command list run (e.g. match_index_count = >6):
	FilterField FirstVertex;
	FilterField FirstIndex;
	FilterField FirstInstance;
	FilterField VertexCount;
	FilterField IndexCount;
	FilterField InstanceCount;

	FilterMatch();
	bool matches(const D3D11_BUFFER_DESC *desc, const DrawCallInfo *call_info) const;
	bool matches(const D3D11_TEXTURE1D_DESC *desc, const DrawCallInfo *call_info) const;
	bool matches(const D3D11_TEXTURE2D_DESC *desc, const DrawCallInfo *call_info) const;
	bool matches(const D3D11_TEXTURE3D_DESC *desc, const DrawCallInfo *call_info) const;
	bool matches(const CustomResource *resource, const DrawCallInfo *call_info) const;

	// Description-only match, ignoring any draw context conditions. Used to
	// build the per-resource fuzzy candidate cache, which is filtered per draw
	// with matches_draw_conditions() afterwards:
	bool matches_desc(const D3D11_BUFFER_DESC *desc) const;
	bool matches_desc(const D3D11_TEXTURE1D_DESC *desc) const;
	bool matches_desc(const D3D11_TEXTURE2D_DESC *desc) const;
	bool matches_desc(const D3D11_TEXTURE3D_DESC *desc) const;
	bool matches_initial_data(bool supplied) const
	{
		return !initial_data_match_set || initial_data_expected == supplied;
	}

	void set_resource_type(D3D11_RESOURCE_DIMENSION type);
	bool update_types_matched();

	// True if any draw context field is constrained.
	bool has_draw_conditions() const;
	// Evaluates the draw context fields against the current draw call. Returns
	// true when there are no draw conditions at all.
	bool matches_draw_conditions(const DrawCallInfo *call_info) const;
};

// Composes [Filter<Name>] sections with the boolean operators accepted inside
// the `->Match(<expression>)` member:
//
//   !   NOT   (prefix, highest precedence)
//   &   AND
//   ^   XOR
//   |   OR    (lowest precedence)
//
// Operands are filter names, resolved against G->mFilters. Each operator is a
// unique glyph - there are no aliases (`&&`, `||`, `~` and word forms are
// syntax errors). Evaluation short-circuits and reuses a single resource fetch
// for the whole tree.
//
// TODO: grouping parentheses are not supported yet - `extract_arguments`
// (CommandList.cpp) scans for the last `(` and needs a balanced scan first.
enum class FilterExprOp {
	LEAF,
	HASH,
	AND,
	OR,
	XOR,
	NOT,
};

class FilterExpr {
public:
	using HashMatcher = std::function<bool(uint64_t)>;
	FilterExprOp op = FilterExprOp::LEAF;

	// Set for LEAF, null otherwise.
	std::shared_ptr<const FilterMatch> leaf;

	// Set for HASH: a raw resource/texture hash literal, e.g.
	// `ps-t0->Match(1234abcd)`. Hashes are singletons - only equality is
	// supported, and multi-hash matching is written with the boolean operators
	// (e.g. `->Match(1234abcd | 89abcdef)`).
	//
	// TODO(shader filters): this will also be compared against the shader hash
	// once the persistent derived shader-data lookup lands, so the resource
	// hash passed to matches() becomes a context-dependent value.
	uint64_t hash = 0;

	// Set for AND/OR/XOR (one or more children) and NOT (exactly one child).
	std::vector<std::shared_ptr<const FilterExpr>> children;

	// True if any leaf in this tree is a HASH literal, so the caller can skip
	// computing a resource hash when it is not actually tested.
	bool has_hash = false;

	// `resource_hash` is null when no hash is available for the target (e.g. a
	// custom resource that has not been hashed), in which case HASH leaves
	// never match.
	bool matches(const uint64_t *resource_hash, const D3D11_BUFFER_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher = nullptr) const;
	bool matches(const uint64_t *resource_hash, const D3D11_TEXTURE1D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher = nullptr) const;
	bool matches(const uint64_t *resource_hash, const D3D11_TEXTURE2D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher = nullptr) const;
	bool matches(const uint64_t *resource_hash, const D3D11_TEXTURE3D_DESC *desc, const DrawCallInfo *call_info, const HashMatcher *hash_matcher = nullptr) const;
	bool matches(const uint64_t *resource_hash, const CustomResource *resource, const DrawCallInfo *call_info, const HashMatcher *hash_matcher = nullptr) const;
};

typedef std::map<std::wstring, std::shared_ptr<FilterMatch>> FilterMatches;
