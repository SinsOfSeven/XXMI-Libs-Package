# LOD Hashes

For INI expressions that request only a particular mip, see [`->MipHash(reverse_mip, array_slice)`](resource-metadata.md#miphashreverse_mip-array_slice). It is independent of the whole-texture override matching described here.

`lod_hash=` lets a `[TextureOverride]` match a Texture2D when **any one** of its mip levels or array slices has the given CRC32C. This is useful for textures created without pixel data and filled later: their ordinary `hash=` may identify only the resource description.

```ini
[Rendering]
track_lod_hashes = 1
track_lod_hash_updates = 1

[TextureOverrideStreamedBody]
lod_hash = 5e6f7a8b
filter_index = 42
```

The override matches the whole texture resource. It does not select or replace only the mip whose CRC matched, and there is no mip index in the override. `filter_index` participates in the same match as other `[TextureOverride]` sections.

For a known resource creation hash, opt in to CPU upload tracking on a regular override:

```ini
[TextureOverrideStreamedFace]
hash = 1b3661bb
track_mips = 1
```

When the creation hash is derived from a texture description, a fuzzy override can select resources by description and by whether the D3D11 creation call supplied `pInitialData`:

```ini
[TextureOverrideEmptyBC7]
match_type = Texture2D
match_format = BC7_UNORM
match_bind_flags = shader_resource
match_misc_flags = 0
match_initial_data = 0
track_mips = 1
```

`match_initial_data = 0` matches a null `pInitialData` argument; `1` matches a non-null argument. It checks the creation argument, not whether the resource's contents are currently empty. Description and initial-data conditions are evaluated once per resource for mip tracking; later writes update the selected resource's cache.

`hash=` is the normal creation-time texture hash. If the texture was created without initial data, it is derived from the resource description; several textures can share it. `track_mips=1` records initial mip CRCs and complete `UpdateSubresource` uploads for resources selected by either a creation hash or a fuzzy description match. Complete copies propagate CRCs when the source has a known CRC. Read/write and write/discard Map shadows can also supply a complete mip; the write/discard shadow starts at zero so unwritten bytes have a defined value. Partial writes and copies from unknown sources leave affected mips unknown. The captured CRCs are shared by `lod_hash=`, `->MipHash(...)`, frame analysis and `->Match(...)`. This opt-in does not scan every texture in the game.

If a game uploads into a separate staging texture and then copies into the selected texture, give the staging texture its own `track_mips=1` override when its creation hash is known. The destination can reuse a complete source CRC; a copy from an untracked source remains unknown. CPU capture runs on immediate contexts. Deferred command lists invalidate the affected hashes and need a later explicit `->MipHash(...)` request to recover an unknown mip.

The CPU capture path excludes textures with render-target, depth-stencil or UAV bind flags, shared-resource flags, or automatic mip generation. Those resources can receive pixels outside the captured upload path. An explicit `->MipHash(...)` request can still use its separate readback fallback.

## Finding a hash

Enable frame analysis and inspect the bound resource in its log. A Texture2D with tracked hashes has a line like:

```text
hash=1a2b3c4d mip_levels=4 populated=2 array=2 lod=[5e6f7a8b@0 90abcdef@5]
```

Each `CRC@subresource` entry gives the value for `lod_hash=` and its subresource index. The index is `mip + array_slice * mip_levels`; for example, `@5` above is mip 1 of array slice 1. `array=` appears only when the texture has more than one slice. Subresources whose contents are not known have no entry; `lod_hash=0` is rejected.

`track_lod_hashes=1` hashes initial data when the texture is created. A texture created empty is marked dirty, and a later override check reads it back after the game fills it. `track_lod_hash_updates=1` also enables tracking of later writes; it can be used alone, but it does not capture the initial data at creation. With `track_lod_hashes=1`, write hooks already mark changed textures dirty, so the second option is not needed for those hooks. Resources selected by `track_mips=1` use the CPU upload cache and do not queue the broad LOD readback.

An INI command checking a dirty texture queues a snapshot of all subresources. On later frames, override checks poll pending snapshots without waiting for the GPU. Matching uses the last completed hashes while a refresh is pending, so a newly populated or changed texture can take several frames to match. Continuously updated textures publish completed snapshots and remain dirty for another refresh. Rechecking an unchanged texture uses the stored hashes.

Each context allows at most 16 pending snapshots and 16 cached staging textures. If the queue is full, a checked texture stays dirty for a later attempt. Completed textures wait until another frame before queuing again so other checked textures can get a turn. Copying still uses GPU bandwidth, and CRC calculation still runs on the calling thread once the data is ready. Hash processing has a soft 1ms budget per frame, checked between subresources; one large mip can exceed that budget. Multisampled textures, checks on deferred contexts, and resources that cannot be copied to a readable staging texture may have no updated LOD hashes.

The `track_lod_hashes` profiler row includes snapshot submission, polling, and CRC calculation, including ordinary texture hash refreshes when `track_texture_updates=1`. It does not include hashing initial data during texture creation. Frame analysis logs completed snapshots; the hashes can describe an earlier frame.

## Matching rules

`lod_hash=` and `hash=` are alternative keys. If both appear in one section, `lod_hash=` takes precedence and the parser warns that `hash=` is ignored.

Draw conditions such as `match_index_count` can narrow a keyed override. Resource description conditions such as `match_width` are only parsed for fuzzy overrides without either hash key; on a keyed override they produce a warning and are not applied. See [Filters](filters.md#relationship-to-texture-overrides) for description matching.

The ordinary texture `hash=` remains unchanged by LOD tracking, so existing hash overrides continue to match as before.
