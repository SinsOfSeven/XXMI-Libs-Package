#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

// Shared storage for opaque payloads and caller-defined metadata. BlobStore
// appends aligned blocks and reads bounded ranges without interpreting their
// contents. Callers own identities, schema validation, invalidation, recovery
// and publication order; changing generations is not a storage-layer policy.
//
// Reads support arbitrary offsets and access order. Windowed reads reuse nearby
// payloads and read ahead after forward progress. Exact reads transfer only the
// requested bytes directly into the caller's buffer. Metadata publication uses
// an atomic file replacement.
//
// Callers must serialize writers and keep logical_end consistent with their
// published metadata. Shared file access alone does not serialize appends.
class BlobStore {
public:
	enum class ReadPolicy { Windowed, Exact };

	// Shader stores retain windowed reads by default. Large resource stores can
	// select Exact to avoid speculative I/O and an extra payload-sized allocation.
	explicit BlobStore(ReadPolicy policy = ReadPolicy::Windowed) : read_policy(policy) {}
	~BlobStore();

	// Identifies the blob file. Must be set before append/read/validate.
	// Handles open lazily; close() or a changed path releases them.
	bool set_path(const wchar_t *path); // returns true when the path changed

	bool is_open() const;
	void close();
	// Make appended payloads durable before publishing their metadata.
	bool flush();
	// Optional writer lease, held on the same handle as I/O until close().
	bool lock_exclusive(bool create);

	// True if the file on disk is at least expected_size bytes long. Used to
	// validate a cache before trusting it - the metadata may only reference
	// payload bytes that already exist:
	bool validate(uint64_t expected_size) const;

	void set_logical_end(uint64_t end);
	uint64_t logical_end() const;

	// Appends one payload block, zero-padded up to block_alignment() bytes so
	// every block always starts at an aligned offset. Returns the new block's
	// offset (always a multiple of block_alignment()):
	bool append(const uint8_t *data, size_t size, uint64_t *out_offset);

	// Reads the complete requested range using this instance's policy. Exact
	// reads use bounded I/O chunks without a window or an additional memcpy.
	// On failure, the caller must discard any partially filled destination:
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
	// The host configures this process-wide target for Windowed stores. Exact
	// stores ignore it. The target is bounded by the setter.
	// Atomic access permits configuration changes alongside reads. Zero disables
	// read-ahead; callers do not need a cache mutex to read this setting.
	static constexpr uint64_t read_ahead_min() { return 0; }
	static constexpr uint64_t read_ahead_max() { return 4 * 1024 * 1024; }
	static constexpr uint64_t read_ahead_default() { return 128 * 1024; }
	static uint64_t clamp_read_ahead(uint64_t v)
	{
		return v < read_ahead_min() ? read_ahead_min()
				: (v > read_ahead_max() ? read_ahead_max() : v);
	}
	static uint64_t read_ahead_size();
	static void set_read_ahead_size(uint64_t size);
	static constexpr uint64_t block_alignment() { return 4096; }

private:
	bool ensure_open(bool create);
	ReadPolicy read_policy;
	static constexpr DWORD exact_read_chunk_size() { return 4 * 1024 * 1024; }

	HANDLE f = INVALID_HANDLE_VALUE;
	bool exclusive_lock = false;
	std::wstring path;

	uint64_t blob_logical_end = 0;
	uint64_t window_start = 0;
	bool window_valid = false;
	std::vector<uint8_t> window;
};

// Writes a metadata file atomically: the data is written to "<path>.tmp" and
// renamed over the real file with write-through, so a crash at any point can
// never leave a half-written cache behind:
bool blob_store_metadata_write(const wchar_t *path, const void *data, size_t size);
// Atomic publication for payload exports without the metadata size cap.
bool blob_store_atomic_write(const wchar_t *path, const void *data, size_t size);

// Reads the entire metadata file into out. The caller owns the format and is
// responsible for validating the size and contents:
bool blob_store_metadata_read(const wchar_t *path, std::vector<uint8_t> *out);

// Bound metadata allocation before trusting counts read from disk. Payload
// blocks have their own bounds and are not subject to this metadata limit.
static constexpr size_t BLOB_STORE_METADATA_MAX = 64 * 1024 * 1024;

inline bool blob_store_range_valid(uint64_t offset, uint64_t size, uint64_t end)
{
	return offset <= end && size <= end - offset;
}

// Deletes a storage file, reporting failures other than a missing file.
void blob_store_delete(const wchar_t *path);
