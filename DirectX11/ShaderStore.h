#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

// Shared on-disk storage machinery for the packed shader caches (the
// ShaderRegex cache today, the ShaderBytecodeRecord ledger next). A cache is
// a pair of files: a small fully-indexed metadata file (loaded wholesale) and
// a grow-only binary blob of shader bytecode. The blob is streamed through a
// small sliding window on demand so it never has to be loaded into memory,
// and both files share the same compaction story: when the cache's generation
// changes the whole pair is wiped and recreated from scratch.
class BlobStore {
public:
	~BlobStore();

	// Identifies the blob file. Must be set before append/read/validate.
	// Handles are opened lazily so a cache that is only ever written to does
	// not keep a file handle open for the lifetime of the process:
	void set_path(const wchar_t *path);

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

	// The blob block alignment and sliding window size. Kept out of the
	// on-disk format (the blob only ever advertises its logical end via the
	// metadata header):
	static constexpr uint64_t block_alignment() { return 4096; }
	static constexpr uint64_t window_size() { return 1 << 20; }

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