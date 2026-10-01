// ZIP archive reader for the VFS. Supports the compression methods we care
// about (stored, deflate, LZMA and zstd); anything else is rejected loudly so
// we never silently produce bad data. The central directory is parsed eagerly
// when an archive is mounted, but file data is only read (and decompressed)
// on demand when an entry is actually requested.
//
// The archive acts as a read-only overlay for the DLL-relative directory it
// lives in: an entry "textures\tex.dds" inside "Mods\MyMod\mod.zip" is
// exposed as "Mods\MyMod\textures\tex.dds".

#include "stdafx.h"
#include "Archive.h"

#include <windows.h>
#include <zstd.h>
#include <miniz.h>
#include <vector>
#include <algorithm>

#include "log.h"
#include "globals.h"
#include "Overlay.h"

#define ZIP_EOCD_SIG 0x06054b50
#define ZIP_CD_SIG   0x02014b50
#define ZIP_LH_SIG   0x04034b50

// Guard against pathological zips / decompression bombs. Configurable via
// [System] max_archive_entry_size (default 64 MiB).
static size_t max_archive_entry_size = 64U * 1024U * 1024U;
static constexpr size_t max_archive_directory_size = 64U * 1024U * 1024U;

void SetMaxArchiveEntrySize(size_t bytes)
{
	max_archive_entry_size = bytes;
}

// LZMA SDK (lzma-23.01/lzma.c) one-shot decode helper:
extern "C" int LzmaDecodeZip(const unsigned char *props, size_t propsSize,
	unsigned char *dest, size_t *destLen,
	const unsigned char *src, size_t *srcLen);

// ZIP LZMA payloads (as written by 7-Zip) start with a header before the
// raw LZMA stream: 2 bytes LZMA version, 1 byte LZMA properties size (5),
// 1 byte (0), then the 5-byte LZMA properties:
#define ZIP_LZMA_HEADER_SIZE 9
#define ZIP_LZMA_PROPS_OFFSET 4
#define ZIP_LZMA_PROPS_SIZE 5

static DWORD ReadU16(const uint8_t *p)
{
	return (DWORD)(p[0] | ((DWORD)p[1] << 8));
}

static DWORD ReadU32(const uint8_t *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static bool HasZip64Marker(DWORD val)
{
	return val == 0xFFFFFFFF;
}

ZipArchive::ZipArchive()
{
	fileHandle = NULL;
}

ZipArchive::~ZipArchive()
{
	Close();
}

void ZipArchive::Close()
{
	if (fileHandle) {
		CloseHandle((HANDLE)fileHandle);
		fileHandle = NULL;
	}
}

// Convert the DLL-relative path we are given into a normalized, lowercased
// key used for all comparisons (ZIP names are case-insensitive on disk and we
// want to match them case-insensitively as well):
static std::wstring NormalizePath(const std::wstring &in)
{
	std::wstring out = in;
	for (wchar_t &c : out) {
		if (c == L'/')
			c = L'\\';
		if (c >= L'A' && c <= L'Z')
			c += L'a' - L'A';
	}
	return out;
}

// An entry is a path relative to its archive's mount directory. Reject names
// that could escape that directory or resolve differently as a Windows path.
static bool SafeEntryName(const std::wstring &name)
{
	if (name.empty() || name.front() == L'\\' || name.find(L':') != std::wstring::npos ||
		name.find(L'\0') != std::wstring::npos)
		return false;
	for (size_t start = 0; start < name.size();) {
		size_t end = name.find(L'\\', start);
		if (end == std::wstring::npos)
			end = name.size();
		size_t length = end - start;
		if (!length || (length == 1 && name[start] == L'.') ||
			(length == 2 && name[start] == L'.' && name[start + 1] == L'.'))
			return false;
		start = end + 1;
	}
	return name.back() != L'\\';
}

// Directory of the DLL (the "3DMigoto path"), always with a trailing '\':
static std::wstring DllDirectory()
{
	wchar_t buf[MAX_PATH] = { 0 };
	DWORD len = GetModuleFileNameW(migoto_handle, buf, MAX_PATH);
	if (!len)
		return L"";
	std::wstring dir(buf, len);
	size_t slash = dir.find_last_of(L'\\');
	if (slash == std::wstring::npos)
		return L"";
	return dir.substr(0, slash + 1);
}

std::wstring Utf8ToWide(const uint8_t *data, size_t size)
{
	// Strip a UTF-8 BOM if present:
	if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		data += 3;
		size -= 3;
	}

	if (size == 0)
		return L"";

	// ZIP names are officially CP437 but virtually everything writes UTF-8;
	// try UTF-8 first and fall back to the ANSI code page:
	UINT code_page = CP_UTF8;
	int wlen = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, (const char*)data, (int)size, NULL, 0);
	if (wlen == 0) {
		code_page = CP_ACP;
		wlen = MultiByteToWideChar(code_page, 0, (const char*)data, (int)size, NULL, 0);
	}
	if (wlen <= 0)
		return L"";

	std::wstring out(wlen, L'\0');
	if (MultiByteToWideChar(code_page, 0, (const char*)data, (int)size, &out[0], wlen) != wlen)
		return L"";
	return out;
}

// Read a chunk of the archive starting at a byte exact offset:
bool ZipArchive::ReadAt(LONGLONG pos, void *buffer, DWORD size) const
{
	std::lock_guard<std::mutex> lock(ioMutex);
	HANDLE handle = (HANDLE)fileHandle;
	LARGE_INTEGER li;
	li.QuadPart = pos;
	if (!SetFilePointerEx(handle, li, NULL, FILE_BEGIN))
		return false;

	DWORD read = 0;
	return ReadFile(handle, buffer, size, &read, NULL) != FALSE && read == size;
}

bool ZipArchive::Open(const wchar_t *p)
{
	path = p;

	HANDLE h = CreateFileW(p, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		LogWarningW(L"  Error opening archive %ls (error %u)\n", p, GetLastError());
		return false;
	}

	fileHandle = (void*)h;

	if (!FindEndOfCentralDirectory() || !ParseCentralDirectory()) {
		Close();
		return false;
	}

	return true;
}

bool ZipArchive::FindEndOfCentralDirectory()
{
	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx((HANDLE)fileHandle, &fileSize) || fileSize.QuadPart < 22)
		return false;

	// The EOCD record is at most 65535 bytes of comment + 22 bytes of header
	// from the end of the file:
	LONGLONG scanSize = fileSize.QuadPart;
	if (scanSize > 65535 + 22)
		scanSize = 65535 + 22;

	std::vector<uint8_t> tail((size_t)scanSize);
	if (!ReadAt(fileSize.QuadPart - scanSize, tail.data(), (DWORD)scanSize))
		return false;

	for (LONGLONG i = scanSize - 22; i >= 0; i--) {
		const uint8_t *p = tail.data() + i;
		if (ReadU32(p) != ZIP_EOCD_SIG)
			continue;

		DWORD commentLen = ReadU16(p + 20);
		if (i + 22 + commentLen != scanSize)
			continue;

		DWORD cdOffset = ReadU32(p + 16);
		DWORD cdSize = ReadU32(p + 12);

		if (HasZip64Marker(cdOffset) || HasZip64Marker(cdSize) ||
			ReadU16(p + 10) == 0xFFFF) {
			LogWarningW(L"  Error opening archive %ls: ZIP64 archives are not supported\n", path.c_str());
			return false;
		}
		if (ReadU16(p + 4) || ReadU16(p + 6) || ReadU16(p + 8) != ReadU16(p + 10)) {
			LogWarningW(L"  Error opening archive %ls: multi-disk ZIP archives are not supported\n", path.c_str());
			return false;
		}

		// ParseCentralDirectory reads these bounds from the archive object.
		eocdOffset = (LONGLONG)cdOffset;
		eocdSize = (LONGLONG)cdSize;
		entryCount = ReadU16(p + 10);

		if (eocdOffset + eocdSize > fileSize.QuadPart) {
			LogWarningW(L"  Error opening archive %ls: corrupt central directory\n", path.c_str());
			return false;
		}

		return true;
	}

	LogWarningW(L"  Error opening archive %ls: no end of central directory record found\n", path.c_str());
	return false;
}

bool ZipArchive::ParseCentralDirectory()
{
	LONGLONG cdSize = eocdSize;
	if (cdSize < 0 || (uint64_t)cdSize > max_archive_directory_size) {
		LogWarningW(L"  Error opening archive %ls: central directory too large\n", path.c_str());
		return false;
	}
	std::vector<uint8_t> cd((size_t)cdSize);
	if (!ReadAt(eocdOffset, cd.data(), (DWORD)cdSize))
		return false;

	size_t pos = 0;
	for (DWORD i = 0; i < entryCount; i++) {
		if (pos + 46 > cd.size())
			return false;

		const uint8_t *p = cd.data() + pos;

		if (ReadU32(p) != ZIP_CD_SIG) {
			LogWarningW(L"  Error opening archive %ls: corrupt central directory\n", path.c_str());
			return false;
		}

		DWORD method = ReadU16(p + 10);
		DWORD csize = ReadU32(p + 20);
		DWORD usize = ReadU32(p + 24);
		DWORD nameLen = ReadU16(p + 28);
		DWORD extraLen = ReadU16(p + 30);
		DWORD commentLen = ReadU16(p + 32);
		DWORD localHeaderPos = ReadU32(p + 42);
		size_t variable_len = (size_t)nameLen + extraLen + commentLen;
		if (variable_len > cd.size() - (pos + 46))
			return false;

		if (HasZip64Marker(csize) || HasZip64Marker(usize) ||
			HasZip64Marker(localHeaderPos)) {
			LogWarningW(L"  Error opening archive %ls: ZIP64 archives are not supported (%ls)\n",
				path.c_str(), Utf8ToWide(cd.data() + pos + 46, (size_t)nameLen).c_str());
			return false;
		}

		pos += 46;

		std::wstring name = Utf8ToWide(cd.data() + pos, (size_t)nameLen);
		pos += variable_len;

		if (name.empty())
			continue;

		ArchiveEntry entry;
		bool isDir = name.back() == L'/' || name.back() == L'\\';
		if (isDir)
			name.pop_back();

		// Normalize to backslashes so we match the rest of the codebase:
		for (wchar_t &c : name)
			if (c == L'/')
				c = L'\\';
		if (!SafeEntryName(name)) {
			LogWarningW(L"  Error opening archive %ls: unsafe entry name\n", path.c_str());
			return false;
		}

		entry.name = name;
		entry.localHeaderPos = localHeaderPos;
		entry.compressedSize = csize;
		entry.uncompressedSize = usize;
		entry.method = (WORD)method;
		entry.isDirectory = isDir;

		if (entryIndex.insert({NormalizePath(name), entries.size()}).second)
			entries.push_back(entry);
	}

	return true;
}

const ArchiveEntry *ZipArchive::FindEntry(const std::wstring &name) const
{
	auto i = entryIndex.find(NormalizePath(name));
	if (i == entryIndex.end())
		return NULL;
	return &entries[i->second];
}

bool ZipArchive::ReadEntry(const ArchiveEntry &entry, std::vector<uint8_t> &out) const
{
	if (entry.isDirectory)
		return false;

	if (entry.compressedSize > max_archive_entry_size ||
		entry.uncompressedSize > max_archive_entry_size) {
		LogWarningW(L"  Error reading %ls from archive %ls: entry too large\n", entry.name.c_str(), path.c_str());
		return false;
	}

	// Read and validate the local file header so we can locate the data:
	uint8_t lh[30];
	if (!ReadAt(entry.localHeaderPos, lh, sizeof(lh))) {
		LogWarningW(L"  Error reading %s from archive %ls: could not read local header\n", entry.name.c_str(), path.c_str());
		return false;
	}

	if (ReadU32(lh) != ZIP_LH_SIG) {
		LogWarningW(L"  Error reading %s from archive %ls: corrupt local header\n", entry.name.c_str(), path.c_str());
		return false;
	}

	DWORD nameLen = ReadU16(lh + 26);
	DWORD extraLen = ReadU16(lh + 28);
	LONGLONG dataPos = entry.localHeaderPos + 30 + nameLen + extraLen;

	std::vector<uint8_t> compressed;
	if (entry.compressedSize == 0 && entry.uncompressedSize != 0) {
		// A zero length file can legitimately have csize 0 and usize 0:
		LogWarningW(L"  Error reading %s from archive %ls: malformed size fields\n", entry.name.c_str(), path.c_str());
		return false;
	}

	if (entry.compressedSize > 0) {
		compressed.resize(entry.compressedSize);
		if (!ReadAt(dataPos, compressed.data(), entry.compressedSize)) {
			LogWarningW(L"  Error reading %s from archive %ls: could not read data\n", entry.name.c_str(), path.c_str());
			return false;
		}
	}

	switch (entry.method) {
	case 0: // stored
		if (entry.compressedSize != entry.uncompressedSize) {
			LogWarningW(L"  Error reading %ls from archive %ls: stored entry size mismatch\n", entry.name.c_str(), path.c_str());
			return false;
		}
		out.assign(compressed.begin(), compressed.end());
		return true;

	case 8: // Deflate (PKWARE method 8)
		if (entry.uncompressedSize == 0) {
			out.clear();
			return true;
		}
		out.resize(entry.uncompressedSize);
		{
			// ZIP deflate streams are raw deflate with no zlib header.
			size_t ret = tinfl_decompress_mem_to_mem(out.data(), out.size(),
				compressed.data(), compressed.size(),
				TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
			if (ret == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || ret != out.size()) {
				LogWarningW(L"  Error decompressing %ls from archive %ls (deflate)\n",
					entry.name.c_str(), path.c_str());
				return false;
			}
		}
		return true;

	case 14: // LZMA (ZIP method 14, as written by 7-Zip)
		if (entry.uncompressedSize == 0) {
			out.clear();
			return true;
		}
		// The payload starts with a 9 byte header: 2 byte LZMA version,
		// 1 byte LZMA properties size (must be 5), 1 byte reserved (0),
		// then the 5 byte LZMA properties followed by the raw stream.
		if (compressed.size() < ZIP_LZMA_HEADER_SIZE ||
			compressed[2] != ZIP_LZMA_PROPS_SIZE ||
			compressed[3] != 0) {
			LogWarningW(L"  Error decompressing %ls from archive %ls: malformed LZMA header\n",
				entry.name.c_str(), path.c_str());
			return false;
		}
		out.resize(entry.uncompressedSize);
		{
			size_t srcLen = compressed.size() - ZIP_LZMA_HEADER_SIZE;
			size_t destLen = out.size();
			int ret = LzmaDecodeZip(compressed.data() + ZIP_LZMA_PROPS_OFFSET, ZIP_LZMA_PROPS_SIZE,
				out.data(), &destLen,
				compressed.data() + ZIP_LZMA_HEADER_SIZE, &srcLen);
			if (ret != 0 || destLen != out.size()) {
				LogWarningW(L"  Error decompressing %ls from archive %ls: LZMA decode failed (error %d)\n",
					entry.name.c_str(), path.c_str(), ret);
				return false;
			}
		}
		return true;

	case 20: // Zstandard (official ZIP method)
	case 93: // Zstandard (legacy pre-rfc alias)
		if (entry.uncompressedSize == 0) {
			out.clear();
			return true;
		}
		out.resize(entry.uncompressedSize);
		{
			size_t ret = ZSTD_decompress(out.data(), out.size(),
				compressed.data(), compressed.size());
			if (ZSTD_isError(ret) || ret != out.size()) {
				LogWarningW(L"  Error decompressing %ls from archive %ls: %hs\n",
					entry.name.c_str(), path.c_str(), ZSTD_getErrorName(ret));
				return false;
			}
		}
		return true;

	default:
		LogOverlayW(LOG_WARNING,
			L"Unsupported compression method %u for entry \"%ls\" in archive %ls\n",
			entry.method, entry.name.c_str(), path.c_str());
		return false;
	}
}

// ---------------------------------------------------------------------------
// Mounted archive registry
// ---------------------------------------------------------------------------

struct MountedArchive {
	std::unique_ptr<ZipArchive> zip;
	std::wstring mountDir;     // Normalized, lowercased, DLL-relative, trailing '\'
	std::wstring mountDirOrig; // Original case, DLL-relative, trailing '\'
	std::wstring archivePath;  // Normalized path of the archive file itself
};

static std::vector<MountedArchive> mountedArchives;

void UnmountAllArchives()
{
	mountedArchives.clear();
}

std::wstring MountArchive(const std::wstring &path)
{
	std::wstring dllDir = DllDirectory();
	std::wstring rel = path;
	bool isAbsolute = (!path.empty() && (path[0] == L'\\' || (path.size() > 1 && path[1] == L':')));

	std::wstring abs = path;
	if (!isAbsolute && !dllDir.empty())
		abs = dllDir + path;

	// Compute the DLL-relative mount directory (the directory the archive
	// "covers" in the virtual filesystem):
	if (isAbsolute && !dllDir.empty()) {
		std::wstring absLower = NormalizePath(abs);
		std::wstring dllLower = NormalizePath(dllDir);
		if (absLower.compare(0, dllLower.size(), dllLower) == 0)
			rel = abs.substr(dllDir.size());
	}

	std::wstring mountDirOrig;
	size_t slash = rel.find_last_of(L'\\');
	if (slash != std::wstring::npos)
		mountDirOrig = rel.substr(0, slash + 1);

	std::wstring mountDir = NormalizePath(mountDirOrig);

	// Guard against double mounting the same archive file:
	std::wstring absNorm = NormalizePath(abs);
	for (const MountedArchive &m : mountedArchives)
		if (m.archivePath == absNorm) {
			LogWarningW(L"  Archive already mounted: %ls\n", path.c_str());
			return L"";
		}

	MountedArchive m;
	m.zip = std::make_unique<ZipArchive>();
	if (!m.zip->Open(abs.c_str()))
		return L"";

	m.mountDir = mountDir;
	m.mountDirOrig = mountDirOrig;
	m.archivePath = absNorm;

	LogInfoW(L"  Mounted archive %ls for directory %ls (%zu entries)\n",
		rel.c_str(), mountDirOrig.c_str(), m.zip->GetEntries().size());

	mountedArchives.push_back(std::move(m));
	return mountDirOrig;
}

// Locates an entry in a mounted archive matching a full filesystem path.
// Returns false if the path exists in no archive. The matching entry (and its
// archive) are returned via optional out parameters without reading or
// decompressing anything.
static bool FindInArchives(const std::wstring &fullPath, ZipArchive **outZip, const ArchiveEntry **outEntry)
{
	std::wstring dllDir = DllDirectory();

	std::wstring logical = fullPath;
	if (!dllDir.empty()) {
		std::wstring fullLower = NormalizePath(fullPath);
		std::wstring dllLower = NormalizePath(dllDir);
		if (fullLower.compare(0, dllLower.size(), dllLower) == 0)
			logical = fullPath.substr(dllDir.size());
	}

	std::wstring lc = NormalizePath(logical);
	bool covered = false;

	for (MountedArchive &m : mountedArchives) {
		const std::wstring &dir = m.mountDir;
		size_t dirSize = dir.size();
		if (dirSize > lc.size())
			continue;
		if (dirSize != 0 && lc.compare(0, dirSize, dir) != 0)
			continue;

		covered = true;
		const ArchiveEntry *entry = m.zip->FindEntry(lc.substr(dirSize));
		if (!entry)
			continue;

		if (outZip)
			*outZip = m.zip.get();
		if (outEntry)
			*outEntry = entry;
		return true;
	}

	// Warn once after checking all archives — avoids noisy per-archive
	// warnings when a directory is backed by multiple archives:
	if (covered && GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		LogWarningW(L"  %ls not found in any archive covering its directory\n", logical.c_str());

	return false;
}

bool ReadFileViaArchives(const std::wstring &fullPath, std::vector<uint8_t> &out)
{
	ZipArchive *zip;
	const ArchiveEntry *entry;
	if (!FindInArchives(fullPath, &zip, &entry)) {
		out.clear();
		return false;
	}

	LogDebugW(L"  VFS: reading %ls from archive delta\n", fullPath.c_str());
	return zip->ReadEntry(*entry, out);
}

bool PathExistsInArchives(const std::wstring &fullPath)
{
	return FindInArchives(fullPath, NULL, NULL);
}

bool IsPathCoveredByArchive(const std::wstring &fullPath)
{
	std::wstring dllDir = DllDirectory();

	std::wstring logical = fullPath;
	if (!dllDir.empty()) {
		std::wstring fullLower = NormalizePath(fullPath);
		std::wstring dllLower = NormalizePath(dllDir);
		if (fullLower.compare(0, dllLower.size(), dllLower) == 0)
			logical = fullPath.substr(dllDir.size());
	}

	std::wstring lc = NormalizePath(logical);
	for (MountedArchive &m : mountedArchives) {
		const std::wstring &dir = m.mountDir;
		size_t dirSize = dir.size();
		if (dirSize > lc.size())
			continue;
		if (dirSize != 0 && lc.compare(0, dirSize, dir) != 0)
			continue;
		return true;
	}
	return false;
}

void ForEachArchiveFile(const std::wstring &mountDirPrefix,
	const std::function<void(const std::wstring &logicalPath, bool isDirectory)> &callback)
{
	std::wstring prefix = NormalizePath(mountDirPrefix);

	for (MountedArchive &m : mountedArchives) {
		if (NormalizePath(m.mountDirOrig) != prefix && m.mountDir != prefix)
			continue;

		for (const ArchiveEntry &entry : m.zip->GetEntries()) {
			if (entry.isDirectory)
				continue;
			callback(m.mountDirOrig + entry.name, false);
		}
	}
}