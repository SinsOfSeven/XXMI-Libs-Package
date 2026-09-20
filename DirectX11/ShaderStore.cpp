#include "ShaderStore.h"

#include <algorithm>
#include <windows.h>

#include "log.h"

static constexpr uint64_t blob_block_alignment = 4096;
static constexpr uint64_t blob_window_size = 1 << 20;

BlobStore::~BlobStore()
{
	close();
}

void BlobStore::set_path(const wchar_t *new_path)
{
	// A device reset or cache wipe closes the handle and will re-open the new
	// path lazily, so just store it:
	if (new_path)
		path = new_path;
	else
		path.clear();
}

bool BlobStore::is_open() const
{
	return f != INVALID_HANDLE_VALUE;
}

void BlobStore::close()
{
	if (f != INVALID_HANDLE_VALUE) {
		CloseHandle(f);
		f = INVALID_HANDLE_VALUE;
	}
	window.clear();
	window_valid = false;
}

bool BlobStore::ensure_open(bool create)
{
	if (f != INVALID_HANDLE_VALUE)
		return true;
	if (path.empty())
		return false;

	f = CreateFile(path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
			create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogWarning("ShaderStore: ensure_open(%S, create=%d) FAILED (last error %u)\n",
				path.c_str(), (int)create, (unsigned)GetLastError());
		return false;
	}
	return true;
}

bool BlobStore::validate(uint64_t expected_size) const
{
	if (!expected_size)
		return true;

	HANDLE h = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER size;
	bool ok = !!GetFileSizeEx(h, &size)
		&& (uint64_t)size.QuadPart >= expected_size;
	CloseHandle(h);

	return ok;
}

void BlobStore::set_logical_end(uint64_t end)
{
	blob_logical_end = end;
}

uint64_t BlobStore::logical_end() const
{
	return blob_logical_end;
}

bool BlobStore::append(const uint8_t *data, size_t size, uint64_t *out_offset)
{
	static const uint8_t zeros[blob_block_alignment] = { 0 };

	if (!ensure_open(true))
		return false;

	// Blocks are padded up to a 4KiB boundary so every block always starts at
	// a 4KiB aligned offset:
	uint64_t block_size = (size + blob_block_alignment - 1)
			& ~(uint64_t)(blob_block_alignment - 1);
	uint64_t offset = blob_logical_end;
	size_t pad = (size_t)(block_size - size);

	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)offset;
	if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN)) {
		LogWarning("ShaderStore: append SetFilePointerEx(%Iu) FAILED (last error %u)\n",
				(uint64_t)offset, (unsigned)GetLastError());
		return false;
	}

	DWORD wrote;
	if (size && (!WriteFile(f, data, (DWORD)size, &wrote, NULL) || wrote != size)) {
		LogWarning("ShaderStore: append WriteFile(%Iu) FAILED (last error %u)\n",
				(uint64_t)size, (unsigned)GetLastError());
		return false;
	}

	while (pad) {
		size_t chunk = (std::min)(pad, sizeof(zeros));
		if (!WriteFile(f, zeros, (DWORD)chunk, &wrote, NULL) || wrote != chunk) {
			LogWarning("ShaderStore: append pad WriteFile FAILED (last error %u)\n",
					(unsigned)GetLastError());
			return false;
		}
		pad -= chunk;
	}

	blob_logical_end = offset + block_size;
	window_start = offset;
	window_valid = false;
	*out_offset = offset;
	return true;
}

bool BlobStore::read(uint64_t offset, uint32_t size, uint8_t *dst)
{
	if (!size)
		return true;
	if (offset + size > blob_logical_end)
		return false;

	// The most recently written blocks stay in the window, so re-reads of a
	// block during the same session (e.g. after a config reload re-linked the
	// command lists) are served straight from memory:
	if (window_valid
	 && offset >= window_start
	 && offset + size <= window_start + window.size()) {
		memcpy(dst, window.data() + (size_t)(offset - window_start), size);
		return true;
	}

	if (!ensure_open(false)) {
		LogWarning("ShaderStore: read open FAILED (offset %Iu size %u)\n",
				(uint64_t)offset, size);
		return false;
	}

	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)offset;
	if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN)) {
		LogWarning("ShaderStore: read SetFilePointerEx(%Iu) FAILED (last error %u)\n",
				(uint64_t)offset, (unsigned)GetLastError());
		return false;
	}

	DWORD got;
	size_t to_read = (size_t)(std::min)(blob_window_size, blob_logical_end - offset);
	window.resize(to_read);
	if (!ReadFile(f, window.data(), (DWORD)to_read, &got, NULL) || got != to_read) {
		LogWarning("ShaderStore: read ReadFile(%Iu@%Iu) FAILED (last error %u)\n",
				(uint64_t)to_read, (uint64_t)offset, (unsigned)GetLastError());
		window_valid = false;
		return false;
	}

	window_start = offset;
	window_valid = true;

	memcpy(dst, window.data(), size);
	return true;
}

bool shader_store_metadata_write(const wchar_t *path, const void *data, size_t size)
{
	std::wstring tmp(path);
	tmp += L".tmp";

	HANDLE f = CreateFile(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogWarning("ShaderStore: metadata write create %S FAILED (last error %u)\n",
				tmp.c_str(), (unsigned)GetLastError());
		return false;
	}

	DWORD written = 0;
	bool ok = !size || (WriteFile(f, data, (DWORD)size, &written, NULL) && written == size);
	CloseHandle(f);

	if (!ok) {
		LogWarning("ShaderStore: metadata write to %S FAILED (size=%Iu, last error %u)\n",
				tmp.c_str(), size, (unsigned)GetLastError());
		DeleteFile(tmp.c_str());
		return false;
	}

	if (!MoveFileEx(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		LogWarning("ShaderStore: metadata rename %S -> %S FAILED (last error %u)\n",
				tmp.c_str(), path, (unsigned)GetLastError());
		return false;
	}
	return true;
}

bool shader_store_metadata_read(const wchar_t *path, std::vector<uint8_t> *out)
{
	out->clear();

	HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;

	DWORD size = GetFileSize(f, NULL);
	DWORD read_bytes = 0;
	bool ok = false;

	if (size != INVALID_FILE_SIZE) {
		out->resize(size);
		ok = ReadFile(f, out->data(), size, &read_bytes, NULL) && read_bytes == size;
	}

	CloseHandle(f);

	if (!ok)
		LogWarning("ShaderStore: metadata read %S FAILED (last error %u)\n", path, (unsigned)GetLastError());

	return ok;
}