#pragma once

// Windows/D3D types first: these headers are sensitive to include order, and
// pulling them in ourselves is what stops a bare <cstdint> ahead of <windows.h>
// from leaving UINT undeclared further down.
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Perceptual and non-perceptual image fingerprints for Texture2D subresources.
//
// The exact per-subresource CRCs in ResourceHash.cpp answer "are these bytes
// identical", which is the right question for a texture the game uploaded once
// and never touches again. They cannot answer "is this the same image": two mip
// levels or differently encoded copies can depict the same source with
// different bytes. That is what this is for.
//
// Everything here works on a single subresource at a time and never assumes a
// mip chain, a resource, or a game-specific texture ordering. A future caller
// should narrow candidates before computing signatures. This library does not
// associate textures with each other; see the notes in ImageHash.cpp.

// One subresource's worth of CPU-visible pixels.
//
// Deliberately non-owning: it points at memory owned by the caller (the game's
// pInitialData during CreateTexture2D, or pSrcData during UpdateSubresource).
// A caller must compute a signature before the owner releases that memory.
struct ImageSurface
{
	const uint8_t *data = nullptr;
	UINT width = 0;
	UINT height = 0;
	size_t row_pitch = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	// Bytes per 4x4 block for a BC format, 0 for uncompressed. Populated from
	// the format so callers do not have to re-derive it. A non-zero value means
	// `data` is a block array with no meaningful row_pitch, exactly as in
	// ResourceHash.cpp.
	UINT block_size = 0;

	bool valid() const { return data && width && height; }
	bool is_compressed() const { return block_size != 0; }
};

// Bits per pixel for the uncompressed formats worth fingerprinting, or 0 for
// anything else (including every compressed format, which is reported through
// ImageSurface::block_size instead). 0 means "we do not know how to read these
// pixels", which callers must treat as unsupported rather than as a match.
UINT ImageFormatBitsPerPixel(DXGI_FORMAT format);

// True if this file can read a surface in the given format: an uncompressed
// format with a known layout, or a compressed format with a block decoder.
// BC1-BC5 and BC7 are decoded; BC6H is not, because it is HDR and its
// tone-mapped luminance would not be comparable to the LDR textures it would be
// matched against.
bool ImageFormatHasDecoder(DXGI_FORMAT format);

// Describes one subresource of a texture without reading any pixels. `data` may
// be null, which produces a surface that is not valid() but still describes
// dimensions. `mip` and `array_slice` are independent; callers holding a
// single subresource index should use
// `init_image_surface_from_subresource` instead.
void init_image_surface_from_desc(const D3D11_TEXTURE2D_DESC *desc, UINT mip,
		UINT array_slice, const uint8_t *data, size_t row_pitch, ImageSurface *surface);

// As above, from a subresource index as returned by D3D11CalcSubresource, for
// the common case of walking a resource's subresources. MipLevels comes from the
// desc, so the index is the only thing the caller needs.
void init_image_surface_from_subresource(const D3D11_TEXTURE2D_DESC *desc, UINT subresource, const uint8_t *data, size_t row_pitch, ImageSurface *surface);

// A fixed-length fingerprint. Small on purpose: the comparison is a distance, so
// a short signature is cheaper to compute and compare, and a coarse one is enough
// to answer "same material" among a handful of candidates. Being able to tell two
// specific pixels apart is explicitly not the goal.
//
// Bits are packed most-significant-bit first, so a 64-bit signature is 8 bytes
// and prints as 16 hex digits. Storing one bit per byte instead would be
// equivalent for comparison but renders as 128 characters of 0 and 1, which is
// indistinguishable from binary on sight and eight times longer in a log.
struct ImageHash
{
	std::vector<uint8_t> bytes;

	bool empty() const { return bytes.empty(); }
	bool operator==(const ImageHash &other) const { return bytes == other.bytes; }
};

// Base class for a fingerprint algorithm. The registry lets a future caller
// choose an algorithm without baking its name into the decoding path.
class IImageHashAlgorithm
{
public:
	virtual ~IImageHashAlgorithm() = default;

	// Registry name, e.g. "avg8".
	virtual const char *name() const = 0;

	// Number of set bits, which for the packing above is the signature length in
	// bits. A 64-bit signature reports 64.
	virtual size_t signature_bits() const = 0;

	// Computes a signature, or returns an empty hash if the surface uses a format
	// this algorithm cannot read. Never throws; an unreadable surface is an empty
	// result, and callers treat empty as "no match", never as "matched".
	virtual ImageHash compute(const ImageSurface &surface) const = 0;

	// Distance between two signatures of this algorithm's length. Lower is more
	// similar. Only meaningful for non-empty hashes of the same size.
	virtual UINT distance(const ImageHash &a, const ImageHash &b) const = 0;
};

// Looks up an algorithm by name, or returns nullptr if there is no such
// algorithm. Never returns nullptr for a name the registry advertises.
const IImageHashAlgorithm *find_image_hash_algorithm(const char *name);

// Names of every registered algorithm, for diagnostics and caller discovery.
std::vector<std::string> image_hash_algorithm_names();

// Computes a signature with the named algorithm, returning an empty hash if the
// name is unknown or the surface is unreadable.
ImageHash compute_image_hash(const char *algorithm, const ImageSurface &surface);

// Computes a signature with every registered algorithm, keyed by name. An
// algorithm that cannot read the surface is simply absent from the result
// rather than present-but-empty, so a caller can tell "computed and this is it"
// from "could not be computed for this format".
std::vector<std::pair<std::string, ImageHash>> compute_all_image_hashes(const ImageSurface &surface);

// A signature rendered as lowercase hex. Empty when the hash is empty.
std::string image_hash_to_hex(const ImageHash &hash);
