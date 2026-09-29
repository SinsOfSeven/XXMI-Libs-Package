#include "ShaderStore.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

#include "log.h"

BlobStore::~BlobStore()
{
	close();
}

bool BlobStore::set_path(const wchar_t *new_path)
{
	const wchar_t *next = new_path ? new_path : L"";
	if (path == next)
		return false;
	close();
	path = next;
	return true;
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
	blob_logical_end = 0;
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
	if (expected_size > INT64_MAX || expected_size % block_alignment())
		return false;
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
	if (end != blob_logical_end)
		window_valid = false;
	blob_logical_end = end;
}

uint64_t BlobStore::logical_end() const
{
	return blob_logical_end;
}

bool BlobStore::append(const uint8_t *data, size_t size, uint64_t *out_offset)
{
	static const uint8_t zeros[block_alignment()] = { 0 };

	if (!size || size > UINT32_MAX || blob_logical_end > INT64_MAX - (uint64_t)size - block_alignment()
	 || blob_logical_end % block_alignment())
		return false;

	if (!ensure_open(true))
		return false;

	// Blocks are padded up to a 4KiB boundary so every block always starts at
	// a 4KiB aligned offset:
	uint64_t block_size = (size + block_alignment() - 1)
			& ~(uint64_t)(block_alignment() - 1);
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
	if (!shader_store_range_valid(offset, size, blob_logical_end) || offset > INT64_MAX)
		return false;

	// The most recently read blocks stay in the window, so re-reads of a
	// block during the same session (e.g. after a config reload re-linked the
	// command lists) are served straight from memory:
	if (window_valid && offset >= window_start
	 && shader_store_range_valid(offset - window_start, size, window.size())) {
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
	// Exact reads avoid copying on-disk padding on isolated requests.
	uint64_t want = size;
	uint64_t ahead = read_ahead_size();
	// Nearby forward misses get speculative read-ahead. Random traffic can also
	// satisfy this heuristic; the configured target bounds that extra I/O.
	// Include the boundary itself so contiguous requests continue reading ahead.
	bool forward = window_valid && offset >= window_start
			&& (offset - window_start) <= (std::max)((uint64_t)window.size(), ahead);
	// The range check proves remaining bytes >= size. Neither the read-ahead
	// target nor EOF can truncate the request, including when ahead is zero.
	size_t to_read = (size_t)(std::min)(forward ? (std::max)(want, ahead) : want,
			blob_logical_end - offset);
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
	if (size > SHADER_STORE_METADATA_MAX)
		return false;
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
	if (ok)
		ok = !!FlushFileBuffers(f);
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

	LARGE_INTEGER size;
	DWORD read_bytes = 0;
	bool ok = false;

	if (GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= SHADER_STORE_METADATA_MAX) {
		out->resize((size_t)size.QuadPart);
		ok = out->empty() || (ReadFile(f, out->data(), (DWORD)out->size(), &read_bytes, NULL)
				&& read_bytes == out->size());
	}

	CloseHandle(f);

	if (!ok)
		LogWarning("ShaderStore: metadata read %S FAILED (last error %u)\n", path, (unsigned)GetLastError());

	return ok;
}

bool shader_store_dxbc_version(const void *data, size_t size, uint32_t *version)
{
	if (!data || size < 32 || size > UINT32_MAX || memcmp(data, "DXBC", 4))
		return false;
	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t total, count;
	memcpy(&total, bytes + 24, 4);
	memcpy(&count, bytes + 28, 4);
	if (total != size || !count || count > (size - 32) / 4)
		return false;
	bool found = false;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t offset, length;
		memcpy(&offset, bytes + 32 + i * 4, 4);
		if (offset < 32 + (uint64_t)count * 4 || !shader_store_range_valid(offset, 8, size))
			return false;
		memcpy(&length, bytes + offset + 4, 4);
		if (!shader_store_range_valid((uint64_t)offset + 8, length, size))
			return false;
		if (!memcmp(bytes + offset, "SHEX", 4) || !memcmp(bytes + offset, "SHDR", 4)) {
			if (length < 8 || length % 4)
				return false;
			uint32_t words;
			memcpy(version, bytes + offset + 8, 4);
			memcpy(&words, bytes + offset + 12, 4);
			if (words != length / 4 || (*version >> 16) > 5)
				return false;
			found = true;
		}
	}
	return found;
}

void shader_store_delete(const wchar_t *path)
{
	if (DeleteFile(path))
		return;

	DWORD err = GetLastError();
	if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
		LogWarning("ShaderStore: delete %S FAILED (last error %u) - stale file will remain\n",
				path, (unsigned)err);
}