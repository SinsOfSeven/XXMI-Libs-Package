#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

// Shared on-disk storage machinery for the packed shader caches (the
// ShaderRegex cache today, the ShaderBytecodeRecord ledger next). A cache is
// a pair of files: a small fully-indexed metadata file (loaded wholesale) and
// a grow-only binary blob of shader bytecode. The blob is read on demand a
// block at a time, so it never has to be loaded into memory as a whole, and
// both files share the same compaction story: when the cache's generation
// changes the whole pair is wiped and recreated from scratch.
//
// Two constraints a caller must respect:
//
//   - Exactly one generation is kept. There is no second generation to fall
//     back to, so a generation change costs a full re-analysis of every shader.
//   - The pair is not safe against two XXMI processes sharing one cache
//     directory. Each process positions its appends at a blob_logical_end read
//     from the metadata it loaded, so the two interleave and overwrite each
//     other's blocks. The failure mode is garbage bytecode, not memory
//     corruption: reads are bounds-checked against blob_logical_end (see read)
//     and again on load, and a garbage block fails at CreateShader. Sharing a
//     cache directory across concurrent instances is therefore unsupported;
//     give each instance its own cache_directory if that ever comes up.
//
// Design intent, since it explains the read policy and is easy to get wrong:
// this cache must serve *any* shader at *any* offset in *any* order, because
// what needs to be fast is a warm cache start against a game's whole shader
// set, not a hot working set. Order-independence is the requirement, not an
// optimisation. Blocks happen to be appended in order of first appearance, so
// a warm start tends to read forward, and read() exploits that as a bonus --
// but only after observing forward progress, so that random order silently
// degrades to one read per block instead of misbehaving. The design
// deliberately does not depend on the access order being knowable in advance.
class BlobStore {
public:
	~BlobStore();

	// Identifies the blob file. Must be set before append/read/validate.
	// Handles are opened lazily so a cache that is only ever written to does
	// not keep a file handle open for the lifetime of the process:
	bool set_path(const wchar_t *path); // returns true when the path changed

	bool is_open() const;
	void close();

	// True if the file on disk is at least expected_size bytes long. Used to
	// validate a cache before trusting it - the metadata may only reference
	// bytecode that already exists:
	bool validate(uint64_t expected_size) const;

	void set_logical_end(uint64_t end);
	uint64_t logical_end() const;

	// Appends one bytecode block, zero-padded up to block_alignment() bytes so
	// every block always starts at an aligned offset. Returns the new block's
	// offset (always a multiple of block_alignment()):
	bool append(const uint8_t *data, size_t size, uint64_t *out_offset);

	// Reads size bytes at offset, served from the sliding window when the
	// range is still resident and otherwise from disk:
	bool read(uint64_t offset, uint32_t size, uint8_t *dst);

	// The blob block alignment and the read-ahead target. Kept out of the
	// on-disk format (the blob only ever advertises its logical end via the
	// metadata header).
	//
	// block_alignment() is load-bearing: append() pads every block up to it, so
	// changing it silently invalidates existing blobs. It shapes the on-disk
	// layout only. read() deliberately ignores it: the window-hit test keys off
	// the requested size, so a rounded-up read cannot turn a miss into a hit and
	// would only inflate the transferred bytes.
	//
	// read_ahead_size() is a *target*, not a limit and not a cap. read() only
	// reads this much when the caller is walking forward from the block it
	// already has resident; an isolated or backwards read fetches just the one
	// block, and a request larger than this is always served in full.
	//
	// The value is a d3dx.ini setting (shader_cache_read_ahead) rather than a
	// constant: the trade-off is workload-dependent and not observable from the
	// cache itself, so it has to be tunable on a real game. It is read straight
	// from a global on the read path, which is free next to the ReadFile the
	// value feeds; that global is std::atomic because this read happens without
	// the cache mutex and a config reload may write it concurrently.
	//
	// Bounded on both ends, because the ini value is an int and untrusted:
	// 0 is a legitimate "no read-ahead" (correct, but reuses nothing), and the
	// upper bound keeps a fat-fingered value from turning one miss into a
	// multi-megabyte read.
	static constexpr uint64_t read_ahead_min() { return 0; }
	static constexpr uint64_t read_ahead_max() { return 4 * 1024 * 1024; }
	static constexpr uint64_t read_ahead_default() { return 128 * 1024; }
	static uint64_t clamp_read_ahead(uint64_t v)
	{
		return v < read_ahead_min() ? read_ahead_min()
				: (v > read_ahead_max() ? read_ahead_max() : v);
	}
	static uint64_t read_ahead_size();
	static constexpr uint64_t block_alignment() { return 4096; }

private:
	bool ensure_open(bool create);

	HANDLE f = INVALID_HANDLE_VALUE;
	std::wstring path;

	uint64_t blob_logical_end = 0;
	uint64_t window_start = 0;
	bool window_valid = false;
	std::vector<uint8_t> window;
};

// Writes a metadata file atomically: the data is written to "<path>.tmp" and
// renamed over the real file with write-through, so a crash at any point can
// never leave a half-written cache behind:
bool shader_store_metadata_write(const wchar_t *path, const void *data, size_t size);

// Reads the entire metadata file into out. The caller owns the format and is
// responsible for validating the size and contents:
bool shader_store_metadata_read(const wchar_t *path, std::vector<uint8_t> *out);

// Bound metadata allocation before trusting any counts read from disk. This is
// not a bytecode limit; individual shaders use the blob's uint32_t size field.
static constexpr size_t SHADER_STORE_METADATA_MAX = 64 * 1024 * 1024;

inline bool shader_store_range_valid(uint64_t offset, uint64_t size, uint64_t end)
{
	return offset <= end && size <= end - offset;
}

// Structural DXBC validation before passing cached bytes to a disassembler or
// driver. Returns the code chunk's version token; does not validate instructions.
bool shader_store_dxbc_version(const void *data, size_t size, uint32_t *version);

// Deletes one file of a cache pair, logging if it could not be removed. A
// missing file is not an error (the common case on a first run). Anything else
// means a stale file will survive a generation wipe, which is benign - the
// .dat gets replaced and the orphaned blob is never referenced again - so this
// only warns rather than failing the wipe.
void shader_store_delete(const wchar_t *path);