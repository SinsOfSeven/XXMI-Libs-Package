# ImageHash algorithm tests

> **Status: reference only.** The perceptual signature system is currently fused
> off the production path. `ImageHash.cpp` / `ImageHash.h` and the vendored
> `bcdec.h` are kept in the tree as a reference implementation, but nothing in the
> proxy calls them any more - see "Fusing off phash" below. Expect most of this to
> need rewriting if it is revived.

Standalone harness for `DirectX11/ImageHash.cpp`. Not part of the solution - it
links `ImageHash.cpp` directly so it can be run without launching the proxy.

`util.h` is an empty stub: `ImageHash.cpp` includes it but uses nothing from it
(`_stricmp` comes from `windows.h`). The stub exists so the real source file can
be compiled unmodified rather than forked.

## Build and run (x64)

```
"%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
cl /nologo /W3 /EHsc /std:c++14 /I. /I..\..\DirectX11 imagehash_test.cpp ..\..\DirectX11\ImageHash.cpp /Fe:imagehash_test.exe
imagehash_test.exe
```

`/I.` puts the local `util.h` stub ahead of the real one at the repo root; the
real one pulls in `crc32c.h` and the rest of the proxy. Exit code 0 means every
check passed.

## What it covers

- Registry lookup, and that an unknown algorithm name returns null.
- Both algorithms produce 64-byte signatures over uncompressed RGBA8.
- Scale invariance: the same gradient at 64x64 and 16x16 is 0 bits apart, which is
  the case an exact CRC cannot handle and the reason this whole path exists.
- The measured relationship between `avg8` and `dhash8`, including the case where
  they overlap. See the comment on `DifferenceHash8` in `ImageHash.cpp`.
- BC1 and BC7 decode to non-degenerate signatures, and block/row-pitch arithmetic
  is correct.
- BC6H, unknown uncompressed formats, a null desc, and null data all yield *no*
  signature rather than an empty or wrong one.

## Note on the BC7 check

The BC7 check is a non-degeneracy check - that 64 random blocks decode to a
varied image, not to a constant one. It would catch a broken mode, partition,
anchor or bit-mangle path, which is the failure mode that matters here. It is not
a bit-exactness test against a reference decoder; if you want that, compare
against a known-good decoder on the same blocks.

The decoder itself is `DirectX11/bcdec.h` (Unlicense / CC0, by Sergii "iOrange"
Kudlai), vendored unmodified.

## Fusing off phash

The signature system was pulled from the active path because the LOD hashes turned
out to answer almost every real case on their own:

- `ResourceHandleInfo::lod_image_hashes` is gone, and with it the signature
  computation that used to run alongside the per-subresource CRC capture.
- The frame analysis line no longer prints `avg8*` / `dhash8*` columns, and the
  `phash=none(...)` marker is gone with them.
- `ResourceHash.cpp` no longer includes `ImageHash.h`.

What remains is the algorithm and decoder source, plus this harness. That code is
still compiled (it is in the vcxproj) so it does not rot, but it has no callers.

Recorded here so a revival does not repeat the dead ends:

- `avg8` and `dhash8` overlap far more than "complementary" suggests. Both average
  within a cell of roughly the same size, so structure finer than a cell is
  smoothed away by both, and a flat field and an equal-mean high-frequency pattern
  are 0 bits apart under each. A second 8x8 gradient hash does not buy the
  fine-structure discrimination that was hoped for; a chroma-aware signature or a
  finer grid would.
- Sub-4x4 mips were a false-positive machine: an 8x8 grid over a 1x1 image gives a
  near-constant result decided by floating point rounding, and those were the most
  common signatures in a real capture.
- The distance threshold was never chosen from real data, so no value of it was
  ever validated.
