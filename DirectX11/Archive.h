#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <mutex>

// A single entry in a ZIP archive, decoded from the central directory.
struct ArchiveEntry {
	std::wstring name;          // Entry name, '/' normalized to '\', original case preserved
	DWORD        localHeaderPos;// Offset of the local file header in the archive
	DWORD        compressedSize;
	DWORD        uncompressedSize;
	WORD         method;        // 0 = stored, 8 = deflate, 14 = LZMA, 20/93 = zstd
	bool         isDirectory;   // Entry name ends with '/'
};

// Random access reader for a ZIP archive that only supports the methods we
// care about: stored (0) and Zstandard (20/93). Every other method is
// rejected with a log warning so we can never silently ship bad data.
class ZipArchive {
public:
	ZipArchive();
	~ZipArchive();

	bool Open(const wchar_t *path);
	void Close();

	const ArchiveEntry *FindEntry(const std::wstring &name) const;
	bool ReadEntry(const ArchiveEntry &entry, std::vector<uint8_t> &out) const;

	const std::wstring& GetPath() const { return path; }
	const std::vector<ArchiveEntry>& GetEntries() const { return entries; }

private:
	bool FindEndOfCentralDirectory();
	bool ParseCentralDirectory();
	bool ReadAt(LONGLONG pos, void *buffer, DWORD size) const;

	std::wstring path;
	void *fileHandle;                       // HANDLE (kept as void* to avoid header order issues)
	mutable std::mutex ioMutex;             // Protects the shared file position and read
	LONGLONG eocdOffset;                    // Central directory offset
	LONGLONG eocdSize;                      // Central directory size
	DWORD entryCount;                       // Entries in the archive
	std::vector<ArchiveEntry> entries;
	std::unordered_map<std::wstring, size_t> entryIndex;  // Normalized (lowercased) name -> entries index
};

// Mounted archive registry. Archives are mounted by their location relative
// to the DLL directory (e.g. "Mods\MyMod\mod.zip"), and act as a read-only
// overlay for that same directory. An entry "textures\tex.dds" inside the
// archive is exposed as "Mods\MyMod\textures\tex.dds".
void UnmountAllArchives();

// Mounts an archive. path may be a full path or a path relative to the DLL
// directory. Returns the DLL-relative mount directory (with trailing '\')
// that the archive's contents map onto, or empty string on failure.
std::wstring MountArchive(const std::wstring &path);

// Tries to read fullPath (a full filesystem path) through any mounted
// archive. Returns true with the decompressed bytes on success, false if the
// path does not exist in any archive.
bool ReadFileViaArchives(const std::wstring &fullPath, std::vector<uint8_t> &out);

// Reads true if fullPath exists in any mounted archive, without reading or
// decompressing the entry. Used to decide path resolution cheaply.
bool PathExistsInArchives(const std::wstring &fullPath);

// Returns true if fullPath lies within the directory mounted by any archive,
// regardless of whether the specific file exists there. Used to decide
// whether an archive is the authoritative source for the path.
bool IsPathCoveredByArchive(const std::wstring &fullPath);

// Sets the maximum compressed/uncompressed size (bytes) an individual archive
// entry may have. Larger entries are refused to guard against pathological
// zips / decompression bombs. Default 64 MiB.
void SetMaxArchiveEntrySize(size_t bytes);

// Invokes callback for every file in the mounted archive whose mount
// directory matches mountDirPrefix (DLL-relative, with trailing '\'). The
// callback is called with the DLL-relative logical path of each entry.
void ForEachArchiveFile(const std::wstring &mountDirPrefix,
	const std::function<void(const std::wstring &logicalPath, bool isDirectory)> &callback);

// Converts UTF-8 bytes (optionally with a BOM) into a wide string.
std::wstring Utf8ToWide(const uint8_t *data, size_t size);