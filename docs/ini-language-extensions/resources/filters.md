# Filters

A `[Filter<Name>]` section declaratively describes **which resources a piece of logic is interested in**. Unlike a fuzzy `[TextureOverride]`, a filter is pure — it does not map to a shader override, it simply evaluates to `1.0` (match) or `0.0` (no match) when a command list runs against a resource.

A filter is evaluated with the `->Filter(<name>)` resource member, which can be used anywhere an expression is accepted, so filters can be tested directly or composed into a score.

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

## Matching custom resources

Filters can be evaluated against `[Resource...]` custom resources as well as live pipeline resources. In that case the filter matches the resource's **declared** description (`type`, `format`, `width`, `height`, `mips`, `array`, `msaa`, `byte_width`, `stride`, `bind_flags`, `misc_flags`), so a filter can identify a custom resource before it is ever substantiated.

## Defaults

Unlike fuzzy texture overrides:

* `match_usage` is **not** defaulted — a filter only constrains the fields it explicitly declares.
* `match_type` is optional. Without it the filter is evaluated against every resource type its constraints are compatible with.

## Usage

`Resource->Filter(<name>)` is an expression operand, where `Resource` is any resource target (`ps-t0`, `ResourceFoo`, `PoolFoo[0]`, …) and `<name>` is the full `[Filter<Name>]` section name. It evaluates to `1.0` on a match, `0.0` when the resource is bound but does not match, and `-0.0` when the resource cannot be resolved (for example an unbound pipeline slot).

```ini
[CommandListSelect]
$is_2k = ps-t0->Filter(Filter2k)

; Compose several filters into a score:
$score = ps-t1->Filter(FilterRatio)
$score = $score + ps-t2->Filter(FilterIndex)
$score = $score * ib->Filter(FilterIndex)

if ResourceFoo->Filter(Filter2k)
    ; Do something
endif
```

Because the result is a normal expression value, filters compose with the rest of the expression language (arithmetic, comparisons, `if` conditions, etc.).

Filters are cheap compared to `store`: they read a resource description (and optionally the current draw call info) without any GPU readback.

See the [Resource Metadata](resource-metadata.md#filter) documentation for the `->Filter(<name>)` member details.

## Refactor note

Fuzzy texture override matching and filters share the same matching core (`FuzzyMatch`), but the two engines currently expose it differently: `FuzzyMatchResourceDesc` stores one `FuzzyMatch` per field, while `FilterMatch` stores a `FilterField` (a list of `FuzzyMatch` constraints that are ANDed) per field.

The intended follow-up refactor is to migrate the fuzzy `TextureOverride` matcher to `FilterField` as well, so that:
* `match_height = >100 & <500` works for texture overrides too.
* Repeated `match_*` keys in a `[TextureOverride]` section are allowed.
* Both engines share a single description-matching implementation.

Until then, `&` chaining and repeated keys are only supported in `[Filter...]` sections.
