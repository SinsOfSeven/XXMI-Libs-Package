# Resource Metadata

Resource metadata can be accessed using two operators:

- `@` returns a numeric identity for the underlying D3D11 resource.
- `->` retrieves resource attributes such as size and stride.

## Resource Identity (`@`)

Returns a numeric (floating point) identity value for the underlying D3D11 resource.
* Can be used with any `ResourceCopyTarget` entity, such as `@ResourceCustom` or `@vs-t0`.
* Returns `0` if no underlying D3D11 resource is available.

```ini
$resource_id_custom = @ResourceFoo
$resource_id_d3d11 = @vs-t0
```

The identity value is derived from the **C++ pointer** to the underlying D3D11 resource. It has **nothing** to do with the resource data itself, so it is not a replacement for data hashes.

Different views of the same texture share this underlying resource identity. All mips and array slices belong to that resource; they do not have separate resource pointers. Different live texture objects have separate identities even if their pixels are identical. Pointer addresses can be reused after destruction, and the encoded identity can collide. Do not treat `@resource` as a permanent content identifier.

> It's essentially a 30-bit hash of 48-bit virtual memory address (standard x86-64 processors typically don't use upper 16 bits). This allows the value to fit into INI variables, which use IEEE-754 floats, where upper 7 bits become the exponent, and lower 23 bits become the mantissa.

### Non-null Resource Check

Resource identity provides an efficient way to check for non-null resource:

```ini
if @ResourceFoo
    ; Do something
endif
```

> It's a lightweight alternative to `ResourceFoo !== null`, which is significantly more expensive.

## Resource Attributes (`->`)

Resource attributes can be accessed using the `->` syntax.

Mip CRCs are queried with [`->Match(hash)`](#match). Use the [`ensuremiphashes` command](../commands/README.md#ensuremiphashes) to request an asynchronous refresh. The cache stores full CRCs and is independent of the buffer-only `cache_resource_data` setting. A frame-analysis entry `lod=[CRC@subresource ...]` uses `subresource = mip + array_slice * mip_levels`.

### `BindFlags`

Returns [D3D11_BIND_FLAGs](../expressions/literals.md#d3d11_bind_flags-enum-literals) of a resource.

```ini
$bind_flags = ResourceFoo->BindFlags
```

### `Offset`

Returns the byte offset of the currently configured buffer region.

For constant buffers, index buffers, and vertex buffers, the offset is derived from the corresponding D3D11 region parameter:

* Constant buffers — `FirstConstant`
* Index buffers — `FirstIndex`
* Vertex buffers — `FirstVertex`

```ini
$offset = vs-cb0->Offset
$offset = ib0->Offset
$offset = vb0->Offset
```

### `Size`

Returns the resource size in bytes.

```ini
$size = ResourceFoo->Size
```

### `Stride`

Returns the resource stride in bytes.

```ini
$stride = ResourceFoo->Stride
```

### `Format`

Returns [DXGI_FORMAT](../expressions/literals.md#dxgi_format-enum-literals) of resource (integer enum value).

```ini
$format = ResourceFoo->Format
```

### `Array`

Returns the array dimension of **texture** resource.

```ini
$array = ResourceTexture->Array
```

### `Mips`

Returns mipmap level count of **texture** resource.

```ini
$mips = ResourceTexture->Mips
```

### `Width`

Returns width of **texture** resource (in [texels](https://en.wikipedia.org/wiki/Texel_(graphics))).

```ini
$width = ResourceTexture->Width
```

### `Height`

Returns height of **texture** resource (in [texels](https://en.wikipedia.org/wiki/Texel_(graphics))).

```ini
$height = ResourceTexture->Height
```

### `SourceStride`

Returns the stride of the source resource used to populate the target **custom** resource.

```ini
[ResourceBar]
type = Buffer
stride = 42
filename = Bar.buf

[PoolFoo]
pool_size = 2
type = Buffer
format = R32_FLOAT

PoolFoo[0] = copy ResourceBar

$bar_stride = PoolFoo[0]->SourceStride ; 42
$foo_stride = PoolFoo[0]->Stride       ; 4
```

Pipeline slots, such as `vb0`, are not supported by `SourceStride`.

### `HashRegion($byte_offset, $byte_size)`

Returns a hash of the buffer contents in the specified region.

* `$byte_offset` — byte offset of the region, relative to the currently configured [buffer region](resource-regions.md) of the target.
* `$byte_size` — size of the region in bytes. A region past the end of the buffer is truncated.

```ini
$object_id = vs-cb1->HashRegion(0, 64) ; Hash of the first 64 bytes visible to the shader.
```

The value is a CRC32C of the data encoded as a 30-bit float, so it fits an INI variable and can be compared or used as a [FIFO pool](../pools/indexing.md#fifo-indexing) key. It is derived from the data itself, unlike the [resource identity](#resource-identity), so equal data in different buffers produces the same hash.

Hashing requires a CPU-side copy of the buffer contents, so the buffer type has to be listed in `cache_resource_data` in the `[Rendering]` section. Hashing a buffer of a type that is not cached returns an [error value](#error-values).

```ini
[Rendering]
cache_resource_data = constant_buffer ; Any of vertex_buffer, index_buffer and constant_buffer, or all, or none.
```

Cached data is captured as the game writes it through `Map` or `UpdateSubresource`, and the hash of a region is then computed once per data change. A buffer that is written on the GPU has to be read back the first time it is hashed, which stalls the pipeline.

> `track_region_hashes = 1` enables the cache for the buffer types it needs itself, so ini files that only set it keep working.

For custom resources, the hash additionally requires that the data was captured from a pipeline slot (`ref` or `copy`). Other custom resources return an [error value](#error-values).

> **Experimental:** `->HashRegion(...)` is experimental and its behavior may change. **Only CB regions** are really tested.

### `SpatialHash($x, $y, $z, $cell_size)`

Returns a **spatial hash** of a world position stored in the buffer: the position is quantized into a 4096×256×4096 grid of cells (X, Y, Z) and the cell coordinates are packed into a single value.

* `$x`, `$y`, `$z` — offsets of the three position floats, in 32-bit elements, relative to the currently configured [buffer region](resource-regions.md) of the target.
* `$cell_size` — size of a grid cell in world units.

```ini
$hash = vs-cb1->SpatialHash(12, 13, 14, 0.5) ; Position at floats 12-14 of the visible region, 0.5 unit cells.
```

Positions within the same cell produce the same hash, and positions in neighboring cells produce hashes that a [spatial pool](../pools/indexing.md#spatial-indexing) can resolve to the same element. The grid wraps around along each axis, so positions `4096` cells apart on X or Z (`256` on Y) collide.

The same data caching rules as for [`HashRegion`](#hashregionbyte_offset-byte_size) apply.

> **Experimental:** `->SpatialHash(...)` is experimental and its behavior may change. **Only CB regions** are really tested.

### `Match`

Evaluates a `[Filter<Name>]` section against the resource and returns `1.0` if it matches, `0.0` if it does not, or `-0.0` if the resource is unbound. The operand is a filter name (the full `[Filter<Name>]` section name, matching is case-insensitive), a raw resource hash (8 or 16 hex digits, no `0x` prefix), or a boolean expression combining several of them with `!` (NOT), `&` (AND), `^` (XOR) and `|` (OR).

```ini
$is_2k = ps-t0->Match(Filter2k)
$is_large = ps-t0->Match(Filter2k | Filter4k)
$is_known = ps-t0->Match(1234abcd)
if ResourceFoo->Match(Filter2k & !Filter4k)
    ; Do something
endif
```

Because it is an ordinary expression operand it can be composed with arithmetic, for example to build a score:

```ini
$score = ps-t1->Match(FilterRatio) + ps-t2->Match(FilterIndex)
```

`->Match(...)` applies the filter's resource and draw call constraints without any GPU readback. A raw hash literal compares against the ordinary resource hash and current cached mip CRCs. For a custom resource, description-only filters use its declared metadata; a hash expression resolves its D3D resource and may instantiate it. See [Filters](filters.md) for the `[Filter<Name>]` section syntax and supported `match_*` fields.

> `->Match(...)` is only valid as an expression operand and cannot be used as a command argument target or an assignment destination.

### Performance

Accessing `->` attributes from custom resources is extremely inexpensive, as the metadata is stored directly with the custom resource.

```ini
ResourceFoo->Size
```

Accessing attributes through pipeline slots is slower because the underlying D3D11 resource must be queried.

```ini
vb0->Size
```

The cost depends on the target and getter. A pipeline slot may require querying the bound D3D11 resource; data getters such as `HashRegion` can also require a GPU readback when cached data is unavailable.

### Error Values

Attribute getters return the following values when data retrieval fails:

| Value   | Meaning                                                   |
| ------- | --------------------------------------------------------- |
| `-1.0f` | `UNKNOWN` — all data sources returned `0`                 |
| `-2.0f` | `RESOURCE_NOT_FOUND` — no buffer was found for the target |
| `-3.0f` | `NOT_A_BUFFER` — the target is not a D3D11 buffer         |
| `-4.0f` | `NOT_A_TEXTURE` — the target is not a texture resource    |
