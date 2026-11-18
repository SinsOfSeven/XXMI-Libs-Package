# Filters

A `[Filter<Name>]` section declaratively describes **which resources a piece of logic is interested in**. Unlike a fuzzy `[TextureOverride]`, a filter is pure - it does not map to a shader override, it simply evaluates to `1.0` (match) or `0.0` (no match) when a command list runs against a resource.

A filter is evaluated with the `->Match(<expression>)` resource member, which can be used anywhere an expression is accepted. It accepts a single filter name, a raw resource hash, or a boolean expression combining several of them, so resources can be tested directly, combined, or composed into a score.

Filter constraints are split into separate matching paths:

* **Description** (`match_type`, `match_format`, dimensions, flags, etc.) - matched against a resource description (or a custom resource's declared description).
* **Draw call** (`match_first_*`, `match_*_count`) - matched against the current draw call context.
* **Perceptual hash** (`match_dhash`, `match_confidence`) - used by slot-filter/hamming distance matching paths, not by `->Match(...)`.

For each named filter leaf in a `->Match(...)` expression, description and draw-call constraints are evaluated as separate paths and both must match (draw constraints additionally require a draw call context).

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

All of the fuzzy `[TextureOverride]` field names are supported. Constraints on resource dimensions (`ByteWidth`/`StructureByteStride` vs. texture fields) automatically restrict which resource types the filter can match.

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

## Perceptual hash fields

These fields drive perceptual (dhash/hamming distance) filter matching paths, such as slot-filter matching used with texture tracking/hamming distance operations. They are independent from `->Match(...)` description/draw evaluation.

| Key | Meaning |
| --- | --- |
| `match_dhash` | Reference perceptual hash |
| `match_confidence` | Minimum required match confidence (clamped to `0.0`-`1.0`, default `0.75`) |

When a filter is used in a slot-filter/hamming path, a **simple singleton** `match_format` constraint can act as an SRV format pre-gate (slots with a different format are skipped early). If `match_format` is undeclared or declared with a complex/non-singleton constraint, no format equality pre-gate is applied there.

## Matching custom resources

Filters can be evaluated against `[Resource...]` custom resources as well as live pipeline resources. In that case the filter matches the resource's **declared** description (`type`, `format`, `width`, `height`, `mips`, `array`, `msaa`, `byte_width`, `stride`, `bind_flags`, `misc_flags`), so a filter can identify a custom resource before it is ever substantiated. Raw hash literals in `->Match(...)` do not match custom resources.

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

`&` and `|` short-circuit, and the whole expression reads the resource description once regardless of how many filters it contains — `ps-t0->Match(A | B)` costs the same as a single `->Match(A)`.

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

8 digits match the current 32-bit texture hash (compared against the resource hash); 16 digits are reserved for the future shader hash. A purely hexadecimal token is never ambiguous with a filter name because a name always carries the `Filter` prefix, which contains non-hex letters. Custom resources (`[Resource...]`) are not content-hashed, so hash literals never match against them.

## Filter field members

A filter's declared fields can be read in expressions as `Filter<Name>-><member>`. This exposes the filter's own constraint metadata (not a matched resource's runtime attributes).

For description and draw-call fields, member access returns a sentinel/value:

| Result | Meaning |
| --- | --- |
| `-1` | Undeclared - the field has no constraints |
| `-5` | Declared but complex/non-singleton-literal - the field cannot be represented as a single plain literal value (for example ranges, multiple/ANDed constraints, or other non-singleton expressions) |
| value | A simple singleton literal value |

These sentinels apply to `Filter<Name>-><member>` access only and are distinct from [resource attribute error values](resource-metadata.md#error-values).

Supported members:

| Member | Path | Corresponding `match_*` key |
| --- | --- | --- |
| `usage` | Desc | `match_usage` |
| `bind_flags` | Desc | `match_bind_flags` |
| `cpu_access_flags` | Desc | `match_cpu_access_flags` |
| `misc_flags` | Desc | `match_misc_flags` |
| `byte_width` | Desc | `match_byte_width` |
| `stride` | Desc | `match_stride` |
| `mips` | Desc | `match_mips` |
| `format` | Desc | `match_format` |
| `width` | Desc | `match_width` |
| `height` | Desc | `match_height` |
| `depth` | Desc | `match_depth` |
| `array` | Desc | `match_array` |
| `msaa` | Desc | `match_msaa` |
| `msaa_quality` | Desc | `match_msaa_quality` |
| `first_vertex` | Draw | `match_first_vertex` |
| `first_index` | Draw | `match_first_index` |
| `first_instance` | Draw | `match_first_instance` |
| `vertex_count` | Draw | `match_vertex_count` |
| `index_count` | Draw | `match_index_count` |
| `instance_count` | Draw | `match_instance_count` |
| `confidence` | Perceptual | `match_confidence` (returned as the confidence value, not a sentinel-based field value) |

`ini
[Filter2k]
match_type = Texture2D
match_width = >=2048
match_format = r8g8b8a8_unorm

[CommandListCheck]
$w = Filter2k->width          ; -5 (declared but non-singleton: >=2048)
$f = Filter2k->format         ; format value when match_format is a simple singleton, else -1/-5
$h = Filter2k->height         ; -1 (undeclared)
$c = Filter2k->confidence     ; 0.75 by default
`

## Usage

`Resource->Match(<expression>)` is an expression operand, where `Resource` is any resource target (`ps-t0`, `ResourceFoo`, `PoolFoo[0]`, …) and `<expression>` is a filter name, a raw resource hash, or a boolean expression over any of them (see [Composing filters](#composing-filters) and [Matching raw hashes](#matching-raw-hashes)). It evaluates to `1.0` on a match, `0.0` when the resource is bound but does not match, and `-0.0` when the resource cannot be resolved (for example an unbound pipeline slot).

For each named filter leaf, the filter's description and draw-call matching paths must both pass (draw constraints require a draw call context). Raw hash leaves compare the resource hash only.

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

Filters are cheap compared to `store`: they read a resource description (and optionally the current draw call info) without any GPU readback.

See the [Resource Metadata](resource-metadata.md#match) documentation for the `->Match(<expression>)` member details.

## Relationship to fuzzy texture overrides

Filters and fuzzy `[TextureOverride]` sections share the same matching primitives and `match_*` grammar. A fuzzy `[TextureOverride]` stores its conditions as separate **description** and **draw-call** match paths (matching how `[Filter<Name>]` sections are split), so description fields drive candidate selection/caching while draw conditions are evaluated per draw call:

```ini
[TextureOverrideExample]
hash = 1234abcd
match_height = >100 & <500
match_index_count = >=6
```

The differences are only in what the section *does* once it matches (a `[TextureOverride]` applies its command lists; a `[Filter]` evaluates to `1.0`/`0.0` or participates in slot/hamming matching) and in the defaults: a fuzzy `[TextureOverride]` defaults `match_usage=default`, whereas a filter does not.
