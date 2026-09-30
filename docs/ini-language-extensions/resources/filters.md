# Filters

A `[Filter<Name>]` section declaratively describes **which resources a piece of logic is interested in**. Unlike a fuzzy `[TextureOverride]`, a filter is pure — it does not map to a shader override, it simply evaluates to `1.0` (match) or `0.0` (no match) when a command list runs against a resource.

A filter is evaluated with the `->Match(<expression>)` resource member, which can be used anywhere an expression is accepted. It accepts a single filter name, a raw resource hash, or a boolean expression combining several of them, so resources can be tested directly, combined, or composed into a score.

## Declaring a filter

```ini
[Filter2k]
match_type = Texture2D
match_width = >=2048
match_height = >100 & <500

[FilterIndex]
match_bind_flags = index_buffer
match_index_count = >6
```

The section name after the `Filter` prefix is the filter's identifier. `[Filter2k]` is referred to as `Filter2k` in command lists (matching is case-insensitive).

## Resource description fields

Resource description fields are shared with fuzzy `[TextureOverride]` sections. Constraints on resource dimensions (`ByteWidth`/`StructureByteStride` vs. texture fields) automatically restrict which resource types the filter can match. `match_initial_data` is specific to fuzzy `[TextureOverride]` sections because it describes the `pInitialData` creation argument, not a resource description; it is not supported in `[Filter<Name>]` sections.

| Key | Applies to |
| --- | --- |
| `match_type` | `Buffer`, `Texture1D`, `Texture2D`, `Texture3D` — restricts the filter to one resource dimension |
| `match_usage` | All — a `D3D11_USAGE_*` value such as `default`, `immutable`, `dynamic`, `staging` |
| `match_bind_flags` | All — bind flag names such as `shader_resource`, `render_target`, `index_buffer` |
| `match_cpu_access_flags` | All |
| `match_misc_flags` | All |
| `match_format` | Textures |
| `match_width`, `match_height`, `match_depth` | Textures |
| `match_mips` | Textures |
| `match_array` | 1D/2D textures |
| `match_msaa`, `match_msaa_quality` | 2D textures |
| `match_byte_width` | Buffers |
| `match_stride` | Structured buffers |

Numeric fields accept the same grammar as fuzzy texture overrides:

```
[ operator ] ( value | ( field_name [ * multiplier ] [ / divider ] ) )

operator   = "=" | "!" | "<" | ">" | "<=" | ">="
field_name = "width" | "height" | "depth" | "array" | "res_width" | "res_height"
```

`res_width` and `res_height` refer to the dimensions of the currently bound render target, which allows aspect ratio matching:

```ini
[FilterRatio]
match_type = Texture2D
match_width = res_height * 16 / 9
```

### Combining constraints

A field may be given more than once, and each value may join several constraints with `&`, so lower and upper bounds can be combined on one line:

```ini
match_height = >100 & <500
```

Every constraint on a field must pass for the field to match.

## Draw call fields

Filters can additionally require values from the current draw call, so a filter can describe "a resource used by a draw call with more than 6 indices":

| Key | Draw call value |
| --- | --- |
| `match_vertex_count` | Number of vertices |
| `match_index_count` | Number of indices |
| `match_instance_count` | Number of instances |
| `match_first_vertex` | First vertex |
| `match_first_index` | First index |
| `match_first_instance` | First instance |

These use the same numeric grammar. If any draw condition is present and the command list is not running inside a draw call, the filter does not match.

## Matching custom resources

Filters can be evaluated against `[Resource...]` custom resources as well as live pipeline resources. In that case the filter matches the resource's **declared** description (`type`, `format`, `width`, `height`, `mips`, `array`, `msaa`, `byte_width`, `stride`, `bind_flags`, `misc_flags`), so a filter can identify a custom resource before it is ever substantiated.

## Defaults

Unlike fuzzy texture overrides:

* `match_usage` is **not** defaulted — a filter only constrains the fields it explicitly declares.
* `match_type` is optional. Without it the filter is evaluated against every resource type its constraints are compatible with.

## Composing filters

`->Match(...)` accepts a boolean expression over filter names, so several filters can be combined without reading the resource more than once:

| Operator | Meaning | Precedence |
| --- | --- | --- |
| `!` | NOT (prefix) | Highest |
| `&` | AND | |
| `^` | XOR | |
| `|` | OR | Lowest |

Each operator is a single, unique glyph — there are no aliases (`&&`, `||`, `~` and word forms such as `and` are not accepted).

```ini
$is_large = ps-t0->Match(Filter2k | Filter4k)   ; 2K or 4K
$is_exact = ps-t0->Match(Filter2k & !Filter4k)  ; 2K and not 4K
$one_of   = ps-t0->Match(FilterA ^ FilterB)     ; exactly one of A, B
```

`&` and `|` short-circuit. The resource description is read once for the expression, but each filter reached by evaluation still checks its own constraints.

> **Note:** The `&` that joins bounds *inside a `match_*` value* (`match_height = >100 & <500`) belongs to the fuzzy-match grammar and is unrelated to the boolean `&` operator described here.

Grouping parentheses are **not** supported yet, so expressions are precedence-only: `->Match((A | B) & C)` is a syntax error. Reorder or split the expression instead.

## Matching raw hashes

A bare resource hash can be matched directly, without declaring a `[Filter...]` section. It must be exactly 8 or 16 hexadecimal digits with no `0x` prefix:

```ini
$is_known = ps-t0->Match(1234abcd)
```

Hashes are singletons and only compare equal, so accepting several of them is done with the boolean operators rather than by listing them together:

```ini
$is_either = ps-t0->Match(1234abcd | 89abcdef)
```

8 digits compare against the ordinary 32-bit resource hash **or any current, cached Texture2D mip CRC32C**. The latter includes mip hashes requested with `->MipHash(reverse_mip, array_slice)` and completed hashes from `track_lod_hashes`. `->Match(...)` never reads a mip back from the GPU, so a mip that has not been cached cannot match yet. It compares the full 8-digit CRC, not the 30-bit value encoded as an INI float by `->MipHash`. A 16-digit hash remains reserved for future shader matching.

```ini
$mip_value = ResourceFoo->MipHash(0, 0) ; Populates the smallest mip's cache if needed.
$matches = ResourceFoo->Match(1234abcd) ; Tests its full CRC and the ordinary hash.
```

The order matters when both lines run in one command list. A subsequent write to the mip invalidates its cached CRC; `->Match(...)` then stops matching it until another `->MipHash(...)` request refreshes it. Description-only filters remain available for `[Resource...]` targets without creating their D3D resource. When evaluation reaches a hash operand on `[Resource...]`, it resolves the actual resource and checks its available hashes; it may instantiate that resource. A purely hexadecimal token is unambiguous because filter names carry the non-hex `Filter` prefix.

## Usage

`Resource->Match(<expression>)` is an expression operand, where `Resource` is any resource target (`ps-t0`, `ResourceFoo`, `PoolFoo[0]`, …) and `<expression>` is a filter name, a raw resource hash, or a boolean expression over any of them (see [Composing filters](#composing-filters) and [Matching raw hashes](#matching-raw-hashes)). It evaluates to `1.0` on a match, `0.0` when the resource is bound but does not match, and `-0.0` when the resource cannot be resolved (for example an unbound pipeline slot).

```ini
[CommandListSelect]
$is_2k = ps-t0->Match(Filter2k)
$is_large = ps-t0->Match(Filter2k | Filter4k)

; Compose several filters into a score:
$score = ps-t1->Match(FilterRatio)
$score = $score + ps-t2->Match(FilterIndex)
$score = $score * ib->Match(FilterIndex)

if ResourceFoo->Match(Filter2k)
    ; Do something
endif
```

Because the result is a normal expression value, filters compose with the rest of the expression language (arithmetic, comparisons, `if` conditions, etc.).

`->Match(...)` itself performs no GPU readback. An earlier `->MipHash(...)` request can block to fill its cache; later `->Match(...)` calls reuse the result.

See the [Resource Metadata](resource-metadata.md#match) documentation for the `->Match(<expression>)` member details.

## Planned: shader matching

Filters currently only match resources. Matching shaders (for example `ps->Match(...)`, or a filter that constrains a shader's declared constant buffers and resources) is planned but not implemented.

The hook points are already marked in the code:

* `ShaderTargetEvaluationMode::RESERVED_SHADER_FILTER` reserves a mode for it.
* `CommandListOperand::process_shader_filter` is where the new mode must be handled, **before** the `mShaderBindingsCache` lookup (which currently assumes the shader is registered).
* `CacheShaderBindings` / `get_shader_bindings_from_bytecode` provide the derived shader data that shader filters will match against. `HackerDevice.cpp`, `ByteCodeReader.cpp` and `globals.h` all carry `TODO(shader filters)` markers.

Filtering will reuse the same `FilterExpr` matcher, with the shader hash (the 16-digit form accepted by `->Match(...)`) and/or the shader's derived bytecode as the match context instead of a resource description.

## Relationship to texture overrides

Filters and fuzzy `[TextureOverride]` sections share a matching implementation and numeric grammar. A fuzzy override has no `hash=` or `lod_hash=` key, so its resource description constraints can be combined just like a filter:

```ini
[TextureOverrideExample]
match_height = >100 & <500
match_index_count = >=6
```

Fuzzy overrides also accept `match_initial_data = 0|1` to distinguish a null `pInitialData` creation argument from a non-null one. This is useful with `track_mips=1` for resources selected by description; see [LOD Hashes](lod-hashes.md).

For a keyed override (`hash=` or `lod_hash=`), the parser applies draw conditions such as `match_index_count`, but does not apply resource description conditions such as `match_height`. It warns if those descriptor keys are present. Use a fuzzy override when matching by description, or a separate `->Match(FilterName)` expression when the script needs both checks. See [LOD Hashes](lod-hashes.md) for the `lod_hash=` key.

A `[TextureOverride]` runs its command lists when it matches; a `[Filter]` evaluates to `1.0` or `0.0`. A fuzzy `[TextureOverride]` defaults to `match_usage=default`, whereas a filter does not.
