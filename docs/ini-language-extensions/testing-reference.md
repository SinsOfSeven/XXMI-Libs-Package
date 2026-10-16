# Testing reference: merged features

This is a quick checklist for testing the current merged resource and shader-cache work. See the linked feature pages for full semantics and edge cases.

## Texture hashes and resource matching

| INI syntax | What it exercises |
| --- | --- |
| `[Rendering] track_texture_updates = 1` | Refresh the ordinary texture hash after updates. |
| `[Rendering] track_lod_hashes = 1` | Hash initial texture mip contents; empty resources can be read back when checked. |
| `[Rendering] track_lod_hash_updates = 1` | Track later writes for LOD hashing; can be used without initial LOD hashing. |
| `[TextureOverrideName] hash = 12345678` | Match a resource by its ordinary creation/content hash. |
| `[TextureOverrideName] lod_hash = 12345678` | Match a Texture2D if any cached mip or array-slice CRC equals the full 32-bit CRC. Takes precedence over `hash=`. |
| `[TextureOverrideName] track_mips = 1` | Opt a known-hash or fuzzy-selected Texture2D into CPU-side mip upload tracking. |
| `[TextureOverrideName] match_type = Texture2D` and `match_format`, `match_bind_flags`, `match_misc_flags`, `match_width`, etc. | Select by resource description without scanning every texture. These are fuzzy overrides when no `hash=`/`lod_hash=` key is present. |
| `[TextureOverrideName] match_initial_data = 0` or `1` | Match whether the creation call's `pInitialData` pointer was null. TextureOverride-only; can be combined with `track_mips = 1`. |
| `ensuremiphashes = ps-t0` or `ensuremiphashes = ps-t[0:6]` | Asynchronously request the full mip chain for one resource or a slot range. `->Match(hash)` sees the mip CRCs after readback completes. |
| `checktextureoverride = ps-t[0:6]` | Check overrides independently for each SRV slot in the inclusive range. |
| `$is_match = ps-t0->Match(12345678)` | Compare an 8-digit literal with the ordinary resource hash or any current, cached mip CRC. This does not read back; use `ensuremiphashes` first if needed. |
| `$is_match = ps-t0->Match(FilterName)` | Evaluate a `[FilterName]` resource-description/draw-call filter. A filter name must contain a non-hex character; hash literals are 8 hex digits. |

For `lod_hash=` and mip-hash debugging, enable frame analysis and inspect `lod=[CRC@subresource ...]`. A subresource index is `mip + array_slice * mip_levels`.

## Cached resource data and range hashes

```ini
[Rendering]
cache_resource_data = constant_buffer ; or vertex_buffer, index_buffer, all, none
track_region_hashes = 1
```

These settings keep CPU-written buffer data in RAM for `store`, `->HashRegion(...)`, and related buffer features. `track_region_hashes` enables the required VB/IB/CB cache itself. GPU-generated contents still need a readback.

## Shader declaration cache

The following shader targets expose declarations parsed from the original shader bytecode:

| Target | Result |
| --- | --- |
| `shader->cb_mask` | Constant-buffer declaration mask. |
| `shader->cb_type(slot)` / `shader->cb_size(slot)` | Declared constant-buffer type and size at a slot. |
| `shader->srv_declares(slot)` | Whether an SRV slot is declared. |
| `shader->srv_type(slot)` / `shader->srv_stride(slot)` / `shader->srv_dimension(slot)` | SRV declaration metadata at a slot. |
| `shader->cb_min/max` / `shader->srv_min/max` | Inclusive declared bounds; `-1` when known empty. |

For consumer behavior, test `checktextureoverride` and texture frame dumping with a shader that declares a slot and one that does not. A known declaration that omits a slot skips that work; missing or unparseable shader metadata preserves the previous behavior.

## Shader regex cache and bytecode validation

`[ShaderRegex...]` sections apply assembly replacements when their patterns match. Test a warm cache hit, a cache miss that assembles a replacement, and malformed replacement bytecode. Cache load/create notices are one-time Info logs; per-shader decisions are Debug logs. Bytecode validation or replacement creation failures should generate an in-game warning overlay.

## Profiling points to watch

- `track_texture_updates`: hits/frame as well as time/frame; check warm static scenes against actively updated textures.
- `track_lod_hashes`: includes asynchronous snapshot submission/polling and CRC work, plus some ordinary texture refresh work; it does not include hashing initial data at creation.
- `ShaderRegex overhead`: compare cold startup/first shader use with a warm run.
- Add separate counters before drawing conclusions about costs: CPU upload CRC time, readback/map time, queue polling/submission, mip-cache hits/misses, and shader declaration-cache hits/misses/parse/persist time.

Detailed behavior: [LOD hashes](resources/lod-hashes.md), [resource metadata](resources/resource-metadata.md), [filters](resources/filters.md), [shader declaration cache](../shader-declaration-cache.md), and [cached `store`](commands/README.md#cached-reads).
