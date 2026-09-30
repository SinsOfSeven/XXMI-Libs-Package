#include "ImageHash.h"
#include "util.h"

// bcdec is a single-header public-domain (Unlicense / CC0) block decompressor.
// Only the implementation half is needed here, in exactly one translation unit.
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

// Why this file deliberately does not group textures
// --------------------------------------------------
// An earlier idea was to associate mip levels by walking descriptions: find
// resources whose dimensions form a power-of-two chain, same format, and treat
// them as one image. That was dropped for two reasons, both of which showed up
// in real games:
//
//   - Shader slot order is not stable. Slots get re-ordered and unused textures
//     get dropped, so anything depending on "this is the third texture" is
//     exactly the per-game analysis this feature exists to avoid.
//   - Descriptions are not unique. Two material maps that look completely
//     different can have byte-identical descriptions, so a 1:1 desc match is not
//     merely unreliable for those two - it groups them *confidently*, producing
//     a wrong answer that looks right.
//
// What we do instead is never ask "which texture is this relative to its
// siblings". A mod declares one reference signature and we ask "which of these
// candidate textures looks like it". Ordering and sibling relationships are not
// needed, and a sloppy candidate filter is harmless because the comparison
// discriminates afterwards.

// Bytes per 4x4 block for the BC formats, matching CompressedFormatBlockSize in
// ResourceHash.cpp. 0 for uncompressed.
static UINT CompressedBlockSize(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return 8;

		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return 16;
	}

	return 0;
}

// Bits per pixel for the uncompressed formats worth fingerprinting, or 0 for
// anything else. 0 means "we do not know how to read these pixels", which
// callers must treat as unsupported rather than as a match. Compressed formats
// report 0 here too and are distinguished by ImageSurface::block_size instead.
UINT ImageFormatBitsPerPixel(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return 32;

		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			return 64;

		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8G8_UNORM:
			return 8;

		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_FLOAT:
			return 16;

		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
			return 32;
	}

	return 0;
}

static void init_image_surface(UINT block_size, UINT width, UINT height, size_t row_pitch, DXGI_FORMAT format, const uint8_t *data, ImageSurface *surface)
{
	surface->data = data;
	surface->width = width;
	surface->height = height;
	surface->row_pitch = row_pitch;
	surface->format = format;
	surface->block_size = block_size;
}

// Block compression decoding
// -------------------------
// BC formats are the common case in real games, so refusing them would have left
// the fingerprint path useless for most textures. Rather than hand-write the BC
// endpoint, partition, anchor and bit-mangle tables - BC7 alone has eight modes
// with per-mode endpoint coding, anchor indices and index bit reordering, and a
// subtle error in any of it produces a confident signature that matches nothing,
// or matches the wrong texture - this delegates to bcdec.
//
// Kept as an opaque 4x4 tile: nothing here knows how a block is coded, only that
// it decodes to 16 pixels, which the shared luminance path then reads exactly as
// it would uncompressed data.

// One decoded 4x4 block, reused for every sample so a decode is amortised across
// the pixels that land in it.
struct DecodedBlock
{
	uint8_t rgba[4 * 4 * 4];
};

// True for the compressed formats whose output can be fed to the luminance path
// below. BC6H is deliberately absent: it decodes to HDR floats, and a perceptual
// hash over tone-mapped luminance at an arbitrary exposure would not be
// comparable to the LDR textures it would be compared against.
static bool bcdec_can_decode(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return true;
	}

	return false;
}

// Decodes one 4x4 block to RGBA8, normalising the one- and two-channel formats to
// grey so the luminance weights always see three real channels.
static bool decode_bc_block(DXGI_FORMAT format, const uint8_t *block, DecodedBlock *out)
{
	memset(out->rgba, 0, sizeof(out->rgba));

	// bcdec writes a 4x4 tile with a caller-chosen pitch, so the tile can be the
	// block itself with no intermediate copy.
	const int PITCH = 4 * 4;

	switch (format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			bcdec_bc1(block, out->rgba, PITCH);
			return true;

		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			bcdec_bc2(block, out->rgba, PITCH);
			return true;

		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			bcdec_bc3(block, out->rgba, PITCH);
			return true;

		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			bcdec_bc7(block, out->rgba, PITCH);
			return true;

		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM: {
			// One channel, 8 bits per pixel, unsigned. bcdec's 3-argument BC4 form
			// is the unsigned decoder; the 4-argument signed one is compiled out
			// unless BCDEC_BC4BC5_PRECISE is set, which also switches BC4/BC5 to a
			// slower exact-interpolation path. Neither is wanted here: SNORM
			// single-channel data read as unsigned would turn negative values
			// into bright edges that are not in the image.
			uint8_t grey[4 * 4];
			bcdec_bc4(block, grey, 4);

			for (int i = 0; i < 16; i++) {
				out->rgba[i * 4 + 0] = grey[i];
				out->rgba[i * 4 + 1] = grey[i];
				out->rgba[i * 4 + 2] = grey[i];
				out->rgba[i * 4 + 3] = 255;
			}
			return true;
		}

		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM: {
			// Two channels, 8 bits each. Games use this for normal maps, so blue
			// is filled with 255 rather than left at zero: a zero there would
			// darken every normal map by a constant factor, which happens to
			// cancel inside a per-image mean but is wrong in principle and wrong
			// for any future per-channel algorithm.
			uint8_t rg[4 * 4 * 2];
			bcdec_bc5(block, rg, 8);

			for (int i = 0; i < 16; i++) {
				out->rgba[i * 4 + 0] = rg[i * 2 + 0];
				out->rgba[i * 4 + 1] = rg[i * 2 + 1];
				out->rgba[i * 4 + 2] = 255;
				out->rgba[i * 4 + 3] = 255;
			}
			return true;
		}
	}

	return false;
}

// True if this file can read a surface in this format: an uncompressed format
// with a known layout, or a compressed format with a decoder. Derived from the
// same functions used to decode, so the answer cannot drift from what is
// actually implemented.
bool ImageFormatHasDecoder(DXGI_FORMAT format)
{
	if (CompressedBlockSize(format) == 0)
		return ImageFormatBitsPerPixel(format) != 0;

	return bcdec_can_decode(format);
}

// Builds a surface description for one subresource without touching the pixels.
// `data` may be null, which yields a surface that is not valid() - used by the
// frame analysis path to report a subresource's dimensions before deciding
// whether it is worth reading.
//
// `array_slice` does not affect any value here: dimensions, pitch and block
// layout are per-mip, not per-slice, and a subresource's pixels are addressed
// relative to its own surface. It is taken so callers holding a (mip, slice) pair
// do not have to know that, and to keep the signature honest if a future format
// ever needs it.
void init_image_surface_from_desc(const D3D11_TEXTURE2D_DESC *desc, UINT mip, UINT array_slice, const uint8_t *data, size_t row_pitch, ImageSurface *surface)
{
	(void)array_slice;

	if (!surface)
		return;

	if (!desc) {
		*surface = ImageSurface();
		return;
	}

	// A mip never reaches zero width or height; D3D11 clamps to 1, and so must
	// we, or the sampling below would divide by zero for a 1x1 base.
	UINT width = max(desc->Width >> mip, 1);
	UINT height = max(desc->Height >> mip, 1);
	UINT block_size = CompressedBlockSize(desc->Format);

	if (block_size) {
		// A compressed subresource is a flat block array: no per-row padding, so
		// row pitch is meaningless. The total size is recorded instead, purely so
		// a caller can sanity-check it.
		size_t blocks_x = (width + 3) / 4;
		size_t blocks_y = (height + 3) / 4;
		row_pitch = blocks_x * blocks_y * block_size;
	}

	init_image_surface(block_size, width, height, row_pitch, desc->Format, data, surface);
}

// As above, from a subresource index as returned by D3D11CalcSubresource. MipLevels
// comes from the desc, so the index is the only thing the caller needs.
void init_image_surface_from_subresource(const D3D11_TEXTURE2D_DESC *desc, UINT subresource, const uint8_t *data, size_t row_pitch, ImageSurface *surface)
{
	UINT mip_levels = desc ? max(desc->MipLevels, 1) : 1;

	init_image_surface_from_desc(desc, subresource % mip_levels, subresource / mip_levels, data, row_pitch, surface);
}

// True if we know how to read this surface's pixels: an uncompressed format with
// a known layout, or a compressed format with a decoder. False means "cannot
// answer", which callers must keep distinct from "compared and did not match".
static bool image_surface_readable(const ImageSurface &surface)
{
	return surface.valid() && ImageFormatHasDecoder(surface.format);
}

// Smallest surface either algorithm will fingerprint, in pixels along one axis.
//
// Below this the signature is not merely weak, it is actively misleading. An 8x8
// grid over a 1x1 image has all 64 cells reading the same value, so "is this cell
// above the mean" has nothing to discriminate and returns a near-constant result.
// Measured in real game data: every 1x1 mip produces an all-zero dHash and a
// checkerboard of all-ones from avg8, decided by floating point rounding rather
// than by image content. Those are the *most common* signatures in the whole
// dataset, so any threshold loose enough to match a real texture also matches
// them - the failure mode is a flood of false positives, not a missed match.
//
// Refusing is the honest answer. A signature is a claim about content, and below
// 4x4 there is no content to make a claim about.
static const UINT MIN_SIGNATURE_DIMENSION = 4;

static bool image_surface_signable(const ImageSurface &surface)
{
	return image_surface_readable(surface) &&
		surface.width >= MIN_SIGNATURE_DIMENSION &&
		surface.height >= MIN_SIGNATURE_DIMENSION;
}

// Luminance of one pixel, for compressed and uncompressed alike.
//
// Every algorithm reads through this, so they all agree on what a pixel's
// brightness is. If one read a compressed block and another read raw bytes, the
// distance between their results would not mean anything, and swapping
// algorithms would silently change which textures match.
//
// For compressed surfaces the containing 4x4 block is decoded on first touch and
// kept until the sampler moves to another block. Sampling walks pixels in
// raster order, so consecutive reads almost always hit the same block and the
// decode is paid once per block rather than once per pixel.
class SurfaceSampler
{
public:
	explicit SurfaceSampler(const ImageSurface &surface)
		: surface_(surface), bpp_(ImageFormatBitsPerPixel(surface.format)),
		cached_block_(-1), block_valid_(false)
	{
		memset(&block_, 0, sizeof(block_));
	}

	float luminance(UINT x, UINT y) const
	{
		if (x >= surface_.width || y >= surface_.height)
			return 0;

		if (surface_.is_compressed())
			return compressed_luminance(x, y);

		return uncompressed_luminance(surface_.data + (size_t)y * surface_.row_pitch, x);
	}

private:
	float uncompressed_luminance(const uint8_t *row, UINT x) const
	{
		switch (surface_.format) {
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return 0.114f * row[x * 4 + 0] + 0.587f * row[x * 4 + 1] + 0.299f * row[x * 4 + 2];

			case DXGI_FORMAT_R8_UNORM:
				return row[x];

			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R16G16_FLOAT: {
				const uint16_t *p = (const uint16_t *)row;
				return (float)p[x * (bpp_ / 16)];
			}

			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_UINT: {
				// Reinterpreted rather than scaled: a float channel is already in
				// 0..1 for the formats that matter. A NaN would poison the mean, so
				// it is treated as black rather than propagated.
				const uint32_t *p = (const uint32_t *)row;
				float f;
				memcpy(&f, &p[x], sizeof(f));
				return (f == f) ? f : 0;
			}
		}

		// Remaining known layouts, addressed by size rather than enumerated.
		const uint8_t *p = row;
		if (bpp_ == 32)
			return 0.114f * p[x * 4 + 0] + 0.587f * p[x * 4 + 1] + 0.299f * p[x * 4 + 2];
		if (bpp_ == 64)
			return 0.114f * p[x * 8 + 0] + 0.587f * p[x * 8 + 2] + 0.299f * p[x * 8 + 4];
		return (float)p[x * (bpp_ / 8)];
	}

	float compressed_luminance(UINT x, UINT y) const
	{
		UINT blocks_x = (surface_.width + 3) / 4;
		int64_t block = (int64_t)(y / 4) * blocks_x + (x / 4);

		if (block != cached_block_) {
			const uint8_t *block_data = surface_.data + (size_t)block * surface_.block_size;
			block_valid_ = decode_bc_block(surface_.format, block_data, &block_);
			cached_block_ = block;
		}

		if (!block_valid_)
			return 0;

		const uint8_t *pixel = &block_.rgba[((y % 4) * 4 + (x % 4)) * 4];
		return 0.114f * pixel[0] + 0.587f * pixel[1] + 0.299f * pixel[2];
	}

	const ImageSurface &surface_;
	UINT bpp_;
	mutable DecodedBlock block_;
	mutable int64_t cached_block_ = -1;
	mutable bool block_valid_ = false;
};

// Mean luminance over a rectangle. Used where a single point would be too
// sensitive to resampling noise: a lower mip is only approximately a downsample
// of its parent, by whatever filter the game chose, so point sampling would make
// every comparison marginal.
static float average_luminance(const SurfaceSampler &sampler, UINT x0, UINT y0, UINT x1, UINT y1)
{
	if (x1 <= x0 || y1 <= y0)
		return 0;

	float total = 0;
	for (UINT y = y0; y < y1; y++) {
		for (UINT x = x0; x < x1; x++)
			total += sampler.luminance(x, y);
	}

	return total / (float)((x1 - x0) * (y1 - y0));
}

// Bit packing
// -----------
// Signatures are built as one bit per decision, then packed most-significant-bit
// first. A 64-bit signature is therefore 8 bytes and prints as 16 hex digits.

// Sets one bit of a signature, growing the backing storage as needed.
static void set_signature_bit(ImageHash &hash, size_t bit, bool value)
{
	if (!value)
		return;

	if (hash.bytes.size() * 8 <= bit)
		hash.bytes.resize(bit / 8 + 1, 0);

	hash.bytes[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
}

// Number of differing bits between two signatures of the same length, which is
// the Hamming distance over the packed representation. Returns (UINT)-1 for a
// length mismatch, so a caller comparing signatures from two different algorithms
// gets "cannot compare" rather than a number that looks meaningful.
static UINT packed_hamming_distance(const ImageHash &a, const ImageHash &b, size_t bits)
{
	if (a.bytes.size() * 8 < bits || b.bytes.size() * 8 < bits)
		return (UINT)-1;

	UINT differing = 0;
	for (size_t i = 0; i * 8 < bits; i++) {
		uint8_t different = (uint8_t)(a.bytes[i] ^ b.bytes[i]);
		// Population count of one byte, without a compiler intrinsic so this
		// behaves identically regardless of build flags.
		while (different) {
			differing += different & 1;
			different >>= 1;
		}
	}

	return differing;
}

// A coarse average-hash: reduce to 8x8 single-channel cells and record whether
// each cell is above or below the mean, the classic average hash.
//
// Two deliberate choices:
//
//   - Comparison is by Hamming distance, not equality, so a re-encoded or
//     rescaled version of the same material still lands close. The texture *kind*
//     is what we are after; exact pixel fidelity is not.
//   - Luminance rather than per-channel hashes. With candidates already bounded
//     to a mesh this is enough to separate materials, and it avoids pretending we
//     can tell a normal map from a roughness mask on content alone - a pair that
//     looks similar in grayscale. Those need the mesh association and a tighter
//     threshold, not a fancier reduction.
//
// No sRGB decode. A lower mip is only approximately a downsample of its parent,
// so an exact match was never available here; skipping the decode keeps this
// usable on CPU data without baking in a colour-space assumption.
class AverageHash8 : public IImageHashAlgorithm
{
public:
	virtual const char *name() const override { return "avg8"; }
	virtual size_t signature_bits() const override { return 64; }

	virtual ImageHash compute(const ImageSurface &surface) const override
	{
		ImageHash hash;

		if (!image_surface_signable(surface))
			return hash;

		const size_t GRID = 8;
		float sums[GRID * GRID] = {};
		size_t samples[GRID * GRID] = {};
		float total = 0;
		size_t total_samples = 0;

		SurfaceSampler sampler(surface);

		for (size_t cell_y = 0; cell_y < GRID; cell_y++) {
			UINT y0 = (UINT)((uint64_t)cell_y * surface.height / GRID);
			UINT y1 = (UINT)((uint64_t)(cell_y + 1) * surface.height / GRID);

			for (size_t cell_x = 0; cell_x < GRID; cell_x++) {
				UINT x0 = (UINT)((uint64_t)cell_x * surface.width / GRID);
				UINT x1 = (UINT)((uint64_t)(cell_x + 1) * surface.width / GRID);

				// A texture smaller than the grid would give an empty cell, which
				// is a division by zero in the mean below. Clamping keeps a real
				// value, and a cell narrower than one pixel is a real answer for a
				// cell that has no detail at this scale.
				if (x1 <= x0)
					x1 = min(x0 + 1, surface.width);
				if (y1 <= y0)
					y1 = min(y0 + 1, surface.height);

				// Bounded sampling: the cell is averaged from at most SAMPLES
				// sub-rectangles rather than every pixel. A 4096x4096 texture would
				// otherwise cost 16 million reads - and for a compressed surface,
				// 16 million block decodes - to produce the same 64 bits as a
				// 64x64 one. Since the output is only 8x8, extra samples per cell
				// change the mean slightly and nothing else.
				const UINT SAMPLES = 4;
				UINT step_x = max((x1 - x0) / SAMPLES, 1);
				UINT step_y = max((y1 - y0) / SAMPLES, 1);
				float cell_sum = 0;
				size_t cell_samples = 0;

				for (UINT y = y0; y < y1; y += step_y) {
					for (UINT x = x0; x < x1; x += step_x) {
						cell_sum += sampler.luminance(x, y);
						cell_samples++;
					}
				}

				size_t cell = cell_y * GRID + cell_x;
				sums[cell] = cell_sum;
				samples[cell] = cell_samples;
				total += cell_sum;
				total_samples += cell_samples;
			}
		}

		if (!total_samples)
			return hash;

		float mean = total / (float)total_samples;
		hash.bytes.assign(GRID * GRID / 8, 0);
		for (size_t cell = 0; cell < GRID * GRID; cell++)
			set_signature_bit(hash, cell, samples[cell] && sums[cell] / (float)samples[cell] > mean);

		return hash;
	}

	virtual UINT distance(const ImageHash &a, const ImageHash &b) const override
	{
		return packed_hamming_distance(a, b, signature_bits());
	}
};

// A gradient hash: reduce to 9x8 cells, then record for each cell whether it is
// brighter than its right-hand neighbour. Also 64 bits, and Hamming distance.
//
// Runs alongside AverageHash8, and the measured relationship between them is
// narrower than it might appear - worth stating plainly rather than overselling:
//
//   - avg8 compares each cell to the image mean, so it responds to tonal skew
//     anywhere in the frame. Measured: a left-to-right ramp is 32 bits away from
//     a flat field.
//   - dHash compares each cell to its right-hand neighbour, so it responds to
//     edge direction and is blind to any brightness that increases monotonically
//     left to right. Measured: that same ramp is 0 bits away, and therefore
//     matches a flat field exactly.
//
// That is a real asymmetry, and it is why running both is worth the cost: dHash
// acts as a check that a match is not resting on tone alone.
//
// But the two are not as independent as "complementary" would suggest. Both
// average within a cell of roughly the same size - 1/8 of the frame for avg8,
// 1/9 by 1/8 for dHash - so structure finer than a cell is smoothed away by
// both. Measured: a flat field and a high-frequency pattern of identical mean are
// 0 bits apart under *both* algorithms. Structure below that scale needs a
// finer grid or a different reduction, not a second 64-bit gradient hash.
class DifferenceHash8 : public IImageHashAlgorithm
{
public:
	virtual const char *name() const override { return "dhash8"; }
	virtual size_t signature_bits() const override { return 64; }

	virtual ImageHash compute(const ImageSurface &surface) const override
	{
		ImageHash hash;

		if (!image_surface_signable(surface))
			return hash;

		// 9x8 rather than 8x8: comparing each cell to its neighbour needs one more
		// column than it has cells, and 8*8 comparisons is what fits in 64 bits.
		const UINT COLS = 9;
		const UINT ROWS = 8;
		float cells[COLS * ROWS];
		SurfaceSampler sampler(surface);

		for (UINT cy = 0; cy < ROWS; cy++) {
			for (UINT cx = 0; cx < COLS; cx++) {
				UINT x0 = (UINT)((uint64_t)cx * surface.width / COLS);
				UINT x1 = (UINT)((uint64_t)(cx + 1) * surface.width / COLS);
				UINT y0 = (UINT)((uint64_t)cy * surface.height / ROWS);
				UINT y1 = (UINT)((uint64_t)(cy + 1) * surface.height / ROWS);

				// Clamped for the same reason as in avg8: a texture narrower than
				// the grid gives an empty cell otherwise, and a repeated cell
				// yields a flat row, which is the honest answer for an image with
				// no detail at that scale.
				if (x1 <= x0)
					x1 = min(x0 + 1, surface.width);
				if (y1 <= y0)
					y1 = min(y0 + 1, surface.height);

				cells[cy * COLS + cx] = average_luminance(sampler, x0, y0, x1, y1);
			}
		}

		hash.bytes.assign(((COLS - 1) * ROWS) / 8, 0);
		size_t bit = 0;
		for (UINT cy = 0; cy < ROWS; cy++) {
			for (UINT cx = 0; cx + 1 < COLS; cx++)
				set_signature_bit(hash, bit++, cells[cy * COLS + cx] > cells[cy * COLS + cx + 1]);
		}

		return hash;
	}

	virtual UINT distance(const ImageHash &a, const ImageHash &b) const override
	{
		return packed_hamming_distance(a, b, signature_bits());
	}
};

// Real objects, not temporaries. `&AverageHash8()` in a static initializer would
// leave a pointer to an object whose lifetime ended with that expression, and
// every virtual call through it would be undefined - which showed up as a crash
// on the first lookup, not as a subtly wrong result.
static AverageHash8 average_hash8;
static DifferenceHash8 difference_hash8;

static const IImageHashAlgorithm *const algorithms[] = {
	&average_hash8,
	&difference_hash8,
};

const IImageHashAlgorithm *find_image_hash_algorithm(const char *name)
{
	if (!name)
		return NULL;

	for (const IImageHashAlgorithm *algorithm : algorithms) {
		if (!_stricmp(algorithm->name(), name))
			return algorithm;
	}

	return NULL;
}

std::vector<std::string> image_hash_algorithm_names()
{
	std::vector<std::string> names;
	for (const IImageHashAlgorithm *algorithm : algorithms)
		names.push_back(algorithm->name());
	return names;
}

ImageHash compute_image_hash(const char *algorithm_name, const ImageSurface &surface)
{
	const IImageHashAlgorithm *algorithm = find_image_hash_algorithm(algorithm_name);
	if (!algorithm)
		return ImageHash();
	return algorithm->compute(surface);
}

std::vector<std::pair<std::string, ImageHash>> compute_all_image_hashes(const ImageSurface &surface)
{
	std::vector<std::pair<std::string, ImageHash>> results;

	for (const IImageHashAlgorithm *algorithm : algorithms) {
		ImageHash hash = algorithm->compute(surface);
		// Skipped rather than recorded as empty, so a caller can tell an algorithm
		// that ran from one that could not read the format.
		if (hash.empty())
			continue;
		results.emplace_back(algorithm->name(), std::move(hash));
	}

	return results;
}

std::string image_hash_to_hex(const ImageHash &hash)
{
	static const char digits[] = "0123456789abcdef";
	std::string hex;

	hex.reserve(hash.bytes.size() * 2);
	for (uint8_t byte : hash.bytes) {
		hex += digits[byte >> 4];
		hex += digits[byte & 0xf];
	}

	return hex;
}
