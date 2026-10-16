# Resource Options

## `override_color_space`

Controls the color space used when loading a texture resource.

By default, the texture's color space is determined by the texture format and/or metadata. 

`override_color_space` allows to explicitly override that behavior:
* `sRGB` — force the texture to be loaded as an sRGB texture.
* `Linear` — force the texture to be loaded as a linear texture.

```ini
[ResourceTexture]
filename = Texture.dds
override_color_space = sRGB
```

## `persist` and `debug_dump`

Set `persist = true` to save a custom resource's current GPU bytes and description
and restore them lazily on a later reload or launch. Both options default to false.

```ini
[ResourceComputed]
persist = true
debug_dump = true
```

Without an explicit `namespace =` assignment, resources from one INI share
`ResourcePersist_<INI filename stem>.dat` and `.blob` beside that INI. With an
explicit namespace, the namespace owns the pair under `ShaderCache` beside the
DLL; INIs with the same namespace share that store. Unsafe namespace characters
are escaped in filenames. Resource persistence works with shader caching disabled.

Buffers, including constant, structured and raw buffers, and single-sample
1D/2D/3D textures, arrays and cubes are supported. Complete underlying bytes are
saved, with buffer region metadata and the current view description. An explicitly
assigned null is persisted without a payload. Never-initialized resources retain
their normal INI initialization behavior.

Observed writes, including compute dispatches, copies, updates, writable maps
and clears, automatically dirty the resource, including writes through aliases.
Reads and bindings alone do not dirty it. Draw/dispatch tracking is conservative:
a possible write counts even if the shader leaves the bytes unchanged. Deferred
draw/dispatch execution conservatively dirties persisted resources on its device.

Use the optional command for changes that bypass normal context hooks, such as
external interop or shared-resource writes:

```ini
[CommandListExternalWrite]
persist_dirty = ResourceComputed
persist_dirty = PoolComputed[0]
; Mark all existing resource elements:
persist_dirty = PoolComputed
```

`persist_dirty` schedules saving at the next checkpoint; it does not perform
immediate readback. Normal tracked compute generation needs no explicit mark.

Dirty resources are saved before configuration resources are destroyed on reload
and during normal swap-chain teardown while the device/context still exist. Clean
restored resources skip readback and disk writes. Crashes, forced termination and
applications that never tear down a swap chain cannot guarantee a final save.

`debug_dump = true` exports `.buf` for buffers and `.dds` for textures beside the
cache when their new state is published. Names include the section/slot identity
and a hash to prevent filename collisions. Null publication removes its old dump.
Dump failure does not invalidate a successful cache save.

Caches are disposable: changing the DLL binary invalidates them automatically,
and incompatible INI constraints or corrupt records fall back to normal resource
initialization. No migration or manual format version is required. Each resource
payload is currently limited to 512 MiB. Multisampled textures, append/consume UAV
counters and resources that cannot be staged are unsupported. Capture or write
failure retains the previous published owner snapshot; outstanding maps must be
unmapped before saving. Blob payloads append over time; compaction is not provided.
