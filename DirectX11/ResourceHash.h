#pragma once

#include <d3d11_1.h>
#include <stdint.h>
#include <tuple>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <atomic>

#include "util.h"
#include "DrawCallInfo.h"
#include "Filter.h"

// A cache-friendly hash table implementation using:
// - Contiguous storage(std::vector)
// - Open addressing(linear probing)
// - No per-element heap allocations
//
// Design goals :
// - Minimize cache misses
// - Avoid pointer chasing
// - Provide predictable performance for real-time systems
// - Keep implementation simple and deterministic
//
// Characteristics :
// - Average O(1) lookup and insert
// - High performance at moderate load factors(<= ~0.5 recommended)
// - Rehashing is O(n) but infrequent if capacity is preallocated
// - Supports backward-shift deletion
//
// Thread safety :
// - Not thread-safe.
template<typename K, typename V, typename Hasher = std::hash<K>>
class FlatHashMap
{
public:
	// Entry represents a single slot in the hash table.
	// 'occupied' indicates whether the slot contains a valid key-value pair.
	// Empty slots are treated as termination points during probing.
	struct Entry {
		K key;
		V value;
		uint32_t generation = 0;
	};

	static size_t NextPow2(size_t v)
	{
		if (v <= 1) return 1;
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		if constexpr (sizeof(size_t) == 8)
			v |= v >> 32;
		v++;
		return v;
	}

	// Constructs the hash map with an initial capacity.
	// - Capacity should ideally be a power of two for optimal performance
	// - Larger initial capacity reduces need for rehashing
	FlatHashMap(size_t initial_capacity = 1024)
	{
		initial_capacity = NextPow2(initial_capacity);
		table.resize(initial_capacity);
		mask = initial_capacity - 1;
		count = 0;
		current_generation = 1; // 0 reserved as "empty"
	}

	// Inserts or updates a key-value pair.
	// Behavior:
	// - If the key already exists, its value is updated
	// - Otherwise, a new entry is inserted
	// Performance:
	// - Amortized O(1)
	// - May trigger rehash if load factor exceeds threshold
	inline void insert(const K& key, const V& value)
	{
		// Maintain load factor <= 0.5
		if ((count * 2) >= table.size()) {
			rehash(table.size() * 2);
		}

		size_t idx = hasher(key) & mask;

		while (true)
		{
			Entry& e = table[idx];

			// Empty slot (generation mismatch)
			if (e.generation != current_generation) {
				e.key = key;
				e.value = value;
				e.generation = current_generation;
				++count;
				return;
			}

			// Update existing
			if (e.key == key) {
				e.value = value;
				return;
			}

			idx = (idx + 1) & mask;
		}
	}

	// Finds a value by key.
	// Returns:
	// - true if found (value written to 'out')
	// - false if not found
	// Performance:
	// - Average O(1)
	// - Depends on load factor and clustering
	inline bool find(const K& key, V& out) const
	{
		size_t idx = hasher(key) & mask;

		// Probe sequence
		while (true)
		{
			const Entry& e = table[idx];

			// Empty slot to key not present
			if (e.generation != current_generation)
				return false;

			if (e.key == key)
			{
				out = e.value;
				return true;
			}

			idx = (idx + 1) & mask;
		}
	}

	// Finds value by key (no copy, faster)
	inline V* find_ptr(const K& key)
	{
		size_t idx = hasher(key) & mask;

		while (true)
		{
			Entry& e = table[idx];

			if (e.generation != current_generation)
				return nullptr;

			if (e.key == key)
				return &e.value;

			idx = (idx + 1) & mask;
		}
	}

	// Erases a key using backward-shift deletion.
	//
	// Behavior:
	// - Removes the key if found
	// - Shifts subsequent entries backward to preserve probe chains
	//
	// Performance:
	// - Average O(1)
	// - Worst case O(cluster length)
	inline bool erase(const K& key)
	{
		size_t idx = hasher(key) & mask;

		// Find the entry
		while (true)
		{
			Entry& e = table[idx];

			if (e.generation != current_generation)
				return false;

			if (e.key == key)
				break;

			idx = (idx + 1) & mask;
		}

		// Remove and compact the cluster
		size_t hole = idx;
		size_t next = (hole + 1) & mask;

		while (true)
		{
			Entry& e = table[next];

			// End of cluster
			if (e.generation != current_generation)
			{
				table[hole].generation = 0;
				break;
			}

			// Determine the natural bucket of this entry
			size_t ideal = hasher(e.key) & mask;

			// Check if this entry can be moved into the hole.
			//
			// If its probe sequence crosses the hole, moving it back
			// keeps lookup correctness.
			bool should_move;

			if (hole <= next)
			{
				// Normal non-wrapping case
				should_move = (ideal <= hole) || (ideal > next);
			}
			else
			{
				// Wrapped cluster
				should_move = (ideal <= hole) && (ideal > next);
			}

			if (should_move)
			{
				table[hole] = std::move(e);
				hole = next;
			}

			next = (next + 1) & mask;
		}

		--count;
		return true;
	}

	// Clears the hash map.
	// Behavior:
	// - Instead of touching memory, we just bump generation
	// - Does NOT shrink capacity
	// - Keeps allocated memory for reuse
	// Performance:
	// - O(1) clear
	inline void clear()
	{
		++current_generation;
		count = 0;

		// Handle rare wraparound (very unlikely: 2^32 clears)
		if (current_generation == 0)
		{
			// Full reset fallback
			for (auto& e : table)
				e.generation = 0;

			current_generation = 1;
		}
	}

	// Returns the number of active entries.
	size_t size() const
	{
		return count;
	}

	// Returns the total capacity of the table.
	size_t capacity() const
	{
		return table.size();
	}

	// Iterates over active entries.
	template<typename F>
	void for_each(F&& fn)
	{
		for (auto& e : table)
		{
			if (e.generation == current_generation)
				fn(e.key, e.value);
		}
	}

private:
	std::vector<Entry> table; // Contiguous storage for cache efficiency
	size_t count = 0;         // Number of active entries
	size_t mask = 0;
	uint32_t current_generation = 1;
	Hasher hasher;

	// Rehashes the table into a larger capacity.
	// Complexity:
	// - O(n)
	// - Should happen infrequently if capacity is sized appropriately
	void rehash(size_t new_capacity)
	{
		// Allocate new table
		std::vector<Entry> old = std::move(table);

		new_capacity = NextPow2(new_capacity);

		table.clear();
		table.resize(new_capacity);

		mask = new_capacity - 1;
		count = 0;

		// Reset generation space
		uint32_t old_generation = current_generation;
		current_generation = 1;

		// Reinsert all valid entries
		for (auto& e : old)
		{
			if (e.generation == old_generation)
			{
				insert(e.key, e.value);
			}
		}
	}
};

struct RegionHashKeyL2
{
	uint32_t offset;
	uint32_t size;

	bool operator==(const RegionHashKeyL2& other) const
	{
		return offset == other.offset && size == other.size;
	}
};

struct RegionHashKeyHasherL2
{
	size_t operator()(const RegionHashKeyL2& k) const
	{
		return (static_cast<uint64_t>(k.offset) << 32) | k.size;
	}
};

struct RegionHashKeyL3
{
	uint64_t ptr;
	uint32_t offset;
	uint32_t size;

	bool operator==(const RegionHashKeyL3& other) const
	{
		return ptr == other.ptr && offset == other.offset && size == other.size;
	}
};

struct RegionHashKeyHasherL3
{
	size_t operator()(const RegionHashKeyL3& k) const
	{
		uint64_t h = k.ptr;            // Use 64-bit pointer as base
		h ^= (uint64_t)k.offset << 32; // XOR offset with upper bits
		h ^= (uint64_t)k.size;         // XOR size with lower bits
		h ^= h >> 32;                  // XOR ptr+size entropy with lower bits
		return h;
	}
};

struct RegionHashesCache
{
	struct RegionCacheEntry {
		uint32_t hash;
		uint32_t version;
	};
public:
	// Data cache invalidation step size in bytes.
	// So, for page size of 256 and buffer size of 16MB we'll get 16MB/256=65536 page count.
	// Page size of 256 looks like a good balance between invalidation precision and memory usage.
	static constexpr UINT PAGE_SIZE = 256;
	// How many region hashes we expect to use per page.
	// Controls initial L2 cache size.
	// So, for 2 hashes per page, page size of 256 and 65536 pages we'll get 65536/(256/2)=512 size.
	// If we're interested in region hashes only as entry points, 512 unique IB/VB0 per 16MB should be a good start.
	static constexpr UINT HASHES_PER_PAGE = 2;

	void Initialize(size_t buffer_size);
	void Add(const RegionHashKeyL2& key, uint32_t hash);
	uint32_t Get(const RegionHashKeyL2& key);
	size_t GetSize();
	void Invalidate(UINT start, UINT end);
	void Clear();

private:
	// Cache of per-region hashes for given buffer.
	// Key = region offset, Value = CRC32 hash of that region.
	// Avoids recomputing hashes for the same draw-call regions.
	std::unique_ptr <FlatHashMap<RegionHashKeyL2, RegionCacheEntry, RegionHashKeyHasherL2>> cache;
	std::vector<UINT> page_versions;
	size_t data_size;
};

struct TextureOverride;

// TextureOverride lookup memoised per resource. The resource description
// never changes, so only the per-draw context filter such as
// match_index_count still runs on each lookup. Lists keep the order of the
// uncached lookup: hash_matches points into G->mTextureOverrideMap and
// fuzzy_matches is the matching subset of G->mFuzzyTextureOverrides.
struct TextureOverrideCandidates
{
	std::vector<TextureOverride>* hash_matches = nullptr;

	// Sections registered under `lod_hash =`, keyed by the CRC of mip 0 rather
	// than by the resource hash. Collects every override whose lod_hash matches
	// any subresource of the resource, so one entry covers the whole mip chain.
	// Null when no section uses lod_hash at all, which is the common case.
	std::vector<TextureOverride*> lod_hash_matches;

	std::vector<TextureOverride*> fuzzy_matches;
};

// Tracks info about specific resource instances:
struct ResourceHandleInfo
{
	D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
	uint32_t hash = 0;
	uint32_t orig_hash = 0;	// Original hash at the time of creation
	uint32_t data_hash = 0;	// Just the data hash for track_texture_updates
	bool data_hash_valid = true;
	bool initial_data_supplied = false;
	bool track_mips = false;

	// Cleared on config reload, rebuilt on next lookup. The hash list is
	// also rebuilt when track_texture_updates changes the hash:
	bool texture_override_candidates_valid = false;
	uint32_t texture_override_hash = 0;
	// The mip cache is filled in progressively as the game streams mips in. Bumped whenever
	// any subresource hash changes, and again on config reload, so the cached
	// list is rebuilt at the next lookup.
	uint32_t lod_candidate_generation = 0;
	uint32_t texture_override_lod_generation = 0;
	TextureOverrideCandidates texture_override_candidates;

	// Set by write hooks and cleared when the requested hashes are current.
	// A texture created with no initial data starts dirty.
	//
	// Selected CPU uploads can refresh this cache directly; legacy tracking can
	// queue a readback at checktextureoverride while this is set.
	bool texture_hashes_dirty = false;
	bool may_have_deferred_writes = false;
	uint64_t texture_hashes_generation = 0;
	uint64_t texture_hashes_readback_id = 0;
	unsigned texture_hashes_last_readback_frame = ~0u;
	struct MipHashCache {
		uint32_t hash = 0;
		uint64_t generation = 0, hashed_generation = 0;
		uint64_t deferred_generation = 0;
		unsigned validation_frame = ~0u;
		bool valid = false;
	};
	// Shared by LOD overrides, explicit mip-hash requests and Match. Indexed by D3D subresource.
	std::vector<MipHashCache> mip_hash_cache;

	// CPU-side copy of the resource data captured via hooks or staging buffer.
	// Used to compute hashes for arbitrary regions without re-mapping
	// the GPU resource multiple times.
	std::shared_ptr<uint8_t[]> cached_data;
	size_t cached_data_offset = 0;
	size_t cached_data_size = 0;
	//uint32_t cached_data_hash = 0;

	std::unique_ptr<RegionHashesCache> region_hashes_cache;

	// TODO: If we are sure we understand all possible differences between
	// the original desc and that obtained by querying the resource we
	// probably don't need to store these. One possible difference is the
	// MipMaps field, which can be set to 0 at creation time to tell DX to
	// create the mip-maps, and I presume it will be filled in by the time
	// we query the desc. Most of the other fields shouldn't change, but
	// I'm not positive about all the misc flags. For now, storing this
	// copy is safer but wasteful.
	union {
		D3D11_TEXTURE2D_DESC desc2D;
		D3D11_TEXTURE3D_DESC desc3D;
	};

	void InitializeDataCache(size_t size, size_t offset = 0);
	void SetDataCache(void* src, size_t size);
	void SetDataCacheRegion(const void* src, size_t size, UINT offset);
	// Clears cached region hashes and invalidates cached buffer data.
	// Should be called when the underlying resource contents change.
	void ClearDataCache();
	uint8_t* GetCachedData();

	void CacheRegionHash(const RegionHashKeyL2& key, uint32_t hash);
	uint32_t GetCachedRegionHash(const RegionHashKeyL2& key);
};

struct CopySubresourceRegionContamination
{
	bool partial;
	UINT DstX;
	UINT DstY;
	UINT DstZ;
	D3D11_BOX SrcBox;

	CopySubresourceRegionContamination() :
		partial(false),
		DstX(0),
		DstY(0),
		DstZ(0),
		SrcBox({0, 0, 0, UINT_MAX, UINT_MAX, UINT_MAX})
	{}

	void Update(bool partial, UINT DstX, UINT DstY, UINT DstZ, const D3D11_BOX *SrcBox)
	{
		this->partial = this->partial || partial;
		this->DstX = max(this->DstX, DstX);
		this->DstY = max(this->DstY, DstY);
		this->DstZ = max(this->DstZ, DstZ);
		if (SrcBox) {
			this->SrcBox.left = max(this->SrcBox.left, SrcBox->left);
			this->SrcBox.top = max(this->SrcBox.top, SrcBox->top);
			this->SrcBox.front = max(this->SrcBox.front, SrcBox->front);

			this->SrcBox.right = min(this->SrcBox.right, SrcBox->right);
			this->SrcBox.bottom = min(this->SrcBox.bottom, SrcBox->bottom);
			this->SrcBox.back = min(this->SrcBox.back, SrcBox->back);
		}
	}
};

// Create a map that uses a hashable tuple of five integers as they key (Hey C++,
// this is something Python can do with what? ... 0 lines of boilerplate?)
typedef std::tuple<uint32_t, UINT, UINT, UINT, UINT> CopySubresourceRegionContaminationMapKey;
template<> struct std::hash<CopySubresourceRegionContaminationMapKey>
{
	size_t operator()(CopySubresourceRegionContaminationMapKey const &key)
	{
		// http://stackoverflow.com/questions/4948780/magic-number-in-boosthash-combine
		size_t seed = 0;
		seed ^= std::hash<uint32_t>()(std::get<0>(key)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<UINT>()(std::get<1>(key)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<UINT>()(std::get<2>(key)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<UINT>()(std::get<3>(key)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= std::hash<UINT>()(std::get<4>(key)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		return seed;
	}
};
typedef std::map<CopySubresourceRegionContaminationMapKey, CopySubresourceRegionContamination>
	CopySubresourceRegionContaminationMap;

// Tracks info about resources by their *original* hash. Primarily for stat collection:
struct ResourceHashInfo
{
	D3D11_RESOURCE_DIMENSION type;
	union {
		D3D11_BUFFER_DESC buf_desc;
		D3D11_TEXTURE1D_DESC tex1d_desc;
		D3D11_TEXTURE2D_DESC tex2d_desc;
		D3D11_TEXTURE3D_DESC tex3d_desc;
	};

	bool initial_data_used_in_hash;
	bool hash_contaminated;
	std::set<UINT> update_contamination;
	std::set<UINT> map_contamination;
	std::set<uint32_t> copy_contamination;
	CopySubresourceRegionContaminationMap region_contamination;

	ResourceHashInfo() :
		type(D3D11_RESOURCE_DIMENSION_UNKNOWN),
		initial_data_used_in_hash(false),
		hash_contaminated(false)
	{}

	struct ResourceHashInfo & operator= (D3D11_BUFFER_DESC desc)
	{
		type = D3D11_RESOURCE_DIMENSION_BUFFER;
		buf_desc = desc;
		return *this;
	}

	struct ResourceHashInfo & operator= (D3D11_TEXTURE1D_DESC desc)
	{
		type = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
		tex1d_desc = desc;
		return *this;
	}

	struct ResourceHashInfo & operator= (D3D11_TEXTURE2D_DESC desc)
	{
		type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
		tex2d_desc = desc;
		return *this;
	}

	struct ResourceHashInfo & operator= (D3D11_TEXTURE3D_DESC desc)
	{
		type = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
		tex3d_desc = desc;
		return *this;
	}
};

typedef std::unordered_map<uint32_t, struct ResourceHashInfo> ResourceInfoMap;

class HackerContext;
class CustomResource;

// This is a COM object that can be attached to a resource via
// ID3D11DeviceChild::SetPrivateDataInterface(), so that when the resource is
// released this class will be as well, giving us a way to reliably know when a
// resource is released and purge it from G->mResources to make sure that if
// the address is later reused we won't use stale data.
//
// This approach is a bit of an experiment - we know the performance of
// Set/GetPrivateData sucks and must be avoided in fast paths, but we never
// look this up so hopefully the overhead will be limited to resource creation
// time where it should be acceptable. An alternative approach would be what we
// do for shaders - purging stale entries after every shader creation anywhere
// in 3DMigoto, but I'm hoping that this will work well enough that we can
// adapt the shaders to use it as well, since the shader approach has a high
// risk of regressions.
//
// Should fix a bug occasionally seen where a custom resource bound to the
// pipeline would act as though it was a game resource when used with
// checktextureoverride, etc.
class ResourceReleaseTracker : public IUnknown
{
	std::atomic_ulong ref;
	ID3D11Resource *resource;

public:
	ResourceReleaseTracker(ID3D11Resource *resource);

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void **ppvObject);
	ULONG STDMETHODCALLTYPE AddRef(void);
	ULONG STDMETHODCALLTYPE Release(void);
};

uint32_t CalcTexture2DDescHash(uint32_t initial_hash, const D3D11_TEXTURE2D_DESC *const_desc);
uint32_t CalcTexture3DDescHash(uint32_t initial_hash, const D3D11_TEXTURE3D_DESC *const_desc);

uint32_t CalcTexture1DDataHash(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData);
uint32_t CalcTexture2DDataHash(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, bool zero_padding = false);
uint32_t CalcTexture2DDataHashAccurate(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData);
std::vector<uint32_t> CalcTexture2DLodHashes(const D3D11_TEXTURE2D_DESC *pDesc,
		const D3D11_SUBRESOURCE_DATA *pInitialData);
uint32_t CalcTexture3DDataHash(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData);

// Marks a resource's tracked hashes (the per-subresource LOD CRCs, and the
// resource-level data hash) as stale. Deliberately cheap: it takes the lock and
// sets a flag, and never reads anything back or computes anything. Safe to call
// from every write hook the game drives.
//
// Deferring the work to EnsureTexture2DHashes is the whole point: hashing a
// texture is expensive and only worth doing for slots a mod actually asks about,
// so the cheap signal is separated from the expensive response.
void MarkTexture2DHashesDirty(ID3D11Resource *resource, UINT subresource = ~0u,
	bool deferred_write = false);
bool TrackTextureMipsForTexture2D(uint32_t hash, const D3D11_TEXTURE2D_DESC *desc, bool initial_data_supplied);
bool ResourceTracksMips(ID3D11Resource *resource);
bool Texture2DCanTrackCPUContents(const D3D11_TEXTURE2D_DESC &desc);
void UpdateTexture2DHashesFromCPU(ID3D11Resource *resource, UINT subresource,
	const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch);
UINT Texture2DHashRowCount(const D3D11_TEXTURE2D_DESC &desc, UINT subresource);
void PropagateTexture2DHashesForCopy(ID3D11Resource *dst, UINT dst_subresource,
	UINT dst_x, UINT dst_y, UINT dst_z, ID3D11Resource *src, UINT src_subresource,
	const D3D11_BOX *src_box);

// Queues a snapshot of a checked dirty texture and polls pending snapshots once
// per frame without waiting for the GPU. Matching uses the last completed hashes
// until a newer snapshot is ready; a newly populated texture may match later.
//
// Refreshes the per-subresource LOD hashes when track_lod_hashes is on, and the
// resource-level data hash when track_texture_updates == 1.
void EnsureTexture2DHashes(HackerContext *context, ID3D11Resource *resource,
	bool force_lod_hashes = false, ResourceHandleInfo *fallback_info = nullptr);
// Reads only completed, current CRCs. Used by ->Match without triggering a
// broad GPU readback; includes legacy LOD CRCs when they are current.
std::vector<uint32_t> GetCachedTexture2DMipHashes(ID3D11Resource *resource,
	ResourceHandleInfo *fallback_info = nullptr);
void RegisterCustomTextureMipHashCache(ID3D11Resource *resource, ResourceHandleInfo *info);
void UnregisterCustomTextureMipHashCache(ID3D11Resource *resource, ResourceHandleInfo *info);

ResourceHandleInfo* GetResourceHandleInfo(ID3D11Resource *resource);
uint32_t GetOrigResourceHash(ID3D11Resource *resource);
uint32_t GetResourceHash(ID3D11Resource *resource);
// Thread-safe wrapper around GetResourceHash() that acquires the critical
// section. Returns 0 if the resource has not been hashed.
uint32_t GetResourceHashSafe(ID3D11Resource *resource);
static bool CacheBufferData(HackerContext* context, ID3D11Buffer* buffer, ResourceHandleInfo* info);
void ClearResourceRegionHashCache(ID3D11Resource* resource);
UINT GetVertexBufferRegionOffset(UINT stride, DrawCallInfo* call_info, UINT byte_offset);
UINT GetIndexBufferRegionOffset(DXGI_FORMAT format, DrawCallInfo* call_info, UINT byte_offset);
UINT GetIndexBufferRegionSize(DXGI_FORMAT format, DrawCallInfo* call_info);
UINT GetVertexBufferRegionSize(UINT stride, DrawCallInfo* call_info);

float BitCastToFloat(uint32_t bits);
uint32_t BitCastToUint(float bits);
uint64_t HashPointer(const void* p);
uint32_t HashUnsigned32(uint32_t u);
float EncodeFloat30(const uint32_t hash);

struct GridPos
{
	uint32_t x;
	uint32_t y;
	uint32_t z;
};
GridPos UnpackCellCoords(uint32_t packed);
uint32_t SpatialDistanceChebyshev(const GridPos& a, const GridPos& b);

uint32_t GetRegionHash(HackerContext* context, ID3D11Buffer* buffer, UINT offset, UINT size, CustomResource* custom_resource = nullptr);
uint32_t GetSpatialHash(HackerContext* context, ID3D11Buffer* buffer, UINT offset_x, UINT offset_y, UINT offset_z, float cell_size = 0.125f, CustomResource* custom_resource = nullptr);

void MarkResourceHashContaminated(ID3D11Resource *dest, UINT DstSubresource,
		ID3D11Resource *src, UINT srcSubresource, char type,
		UINT DstX, UINT DstY, UINT DstZ, const D3D11_BOX *SrcBox);

void UpdateResourceHashFromCPU(ID3D11Resource *resource,
	const void *data, UINT rowPitch, UINT depthPitch);

bool MapTrackResourceHashUpdate(ID3D11Resource *pResource, UINT Subresource);

int StrResourceDesc(char *buf, size_t size, const D3D11_BUFFER_DESC *desc);
int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE1D_DESC *desc);
int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE2D_DESC *desc);
int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE3D_DESC *desc);
int StrResourceDesc(char *buf, size_t size, struct ResourceHashInfo &info);

void LogResourceDesc(const D3D11_BUFFER_DESC *desc);
void LogResourceDesc(const D3D11_TEXTURE1D_DESC *desc);
void LogResourceDesc(const D3D11_TEXTURE2D_DESC *desc);
void LogResourceDesc(const D3D11_TEXTURE3D_DESC *desc);
void LogResourceDesc(ID3D11Resource *resource);
template <typename DescType>
static void LogDebugResourceDesc(DescType *desc)
{
	if (gLogDebug)
		LogResourceDesc(desc);
}
void LogViewDesc(const D3D11_SHADER_RESOURCE_VIEW_DESC *desc);
void LogViewDesc(const D3D11_RENDER_TARGET_VIEW_DESC *desc);
void LogViewDesc(const D3D11_DEPTH_STENCIL_VIEW_DESC *desc);
void LogViewDesc(const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc);
template <typename DescType>
static void LogDebugViewDesc(DescType *desc)
{
	if (gLogDebug)
		LogViewDesc(desc);
}


// -----------------------------------------------------------------------------------------------
//                       Fuzzy Texture Override Matching Support
// -----------------------------------------------------------------------------------------------

// https://msdn.microsoft.com/en-us/library/windows/desktop/ff476202(v=vs.85).aspx
static wchar_t *ResourceDimensions[] = {
	L"UNKNOWN",
	L"BUFFER",
	L"TEXTURE1D",
	L"TEXTURE2D",
	L"TEXTURE3D",
};

// https://msdn.microsoft.com/en-us/library/windows/desktop/ff476259(v=vs.85).aspx
static wchar_t *ResourceUsage[] = {
	L"DEFAULT",
	L"IMMUTABLE",
	L"DYNAMIC",
	L"STAGING"
};
static wchar_t *TexResourceUsage(UINT usage)
{
	if (usage < sizeof(ResourceUsage) / sizeof(ResourceUsage[0]))
		return ResourceUsage[usage];
	return L"UNKNOWN";
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/ff476106(v=vs.85).aspx
enum class ResourceCPUAccessFlags {
	INVALID = 0,
	WRITE   = 0x00010000,
	READ    = 0x00020000,
};
SENSIBLE_ENUM(ResourceCPUAccessFlags);
static EnumName_t<const wchar_t *, ResourceCPUAccessFlags> ResourceCPUAccessFlagNames[] = {
	{L"write", ResourceCPUAccessFlags::WRITE},
	{L"read", ResourceCPUAccessFlags::READ},
	{NULL, ResourceCPUAccessFlags::INVALID} // End of list marker
};

enum class ResourceMiscFlags {
	INVALID                          = 0,
	GENERATE_MIPS                    = 0x00000001,
	SHARED                           = 0x00000002,
	TEXTURECUBE                      = 0x00000004,
	DRAWINDIRECT_ARGS                = 0x00000010,
	BUFFER_ALLOW_RAW_VIEWS           = 0x00000020,
	BUFFER_STRUCTURED                = 0x00000040,
	RESOURCE_CLAMP                   = 0x00000080,
	SHARED_KEYEDMUTEX                = 0x00000100,
	GDI_COMPATIBLE                   = 0x00000200,
	SHARED_NTHANDLE                  = 0x00000800,
	RESTRICTED_CONTENT               = 0x00001000,
	RESTRICT_SHARED_RESOURCE         = 0x00002000,
	RESTRICT_SHARED_RESOURCE_DRIVER  = 0x00004000,
	GUARDED                          = 0x00008000,
	TILE_POOL                        = 0x00020000,
	TILED                            = 0x00040000,
	HW_PROTECTED                     = 0x00080000,
};
SENSIBLE_ENUM(ResourceMiscFlags);
static EnumName_t<const wchar_t *, ResourceMiscFlags> ResourceMiscFlagNames[] = {
	{L"generate_mips", ResourceMiscFlags::GENERATE_MIPS},
	{L"shared", ResourceMiscFlags::SHARED},
	{L"texturecube", ResourceMiscFlags::TEXTURECUBE},
	{L"drawindirect_args", ResourceMiscFlags::DRAWINDIRECT_ARGS},
	{L"buffer_allow_raw_views", ResourceMiscFlags::BUFFER_ALLOW_RAW_VIEWS},
	{L"buffer_structured", ResourceMiscFlags::BUFFER_STRUCTURED},
	{L"resource_clamp", ResourceMiscFlags::RESOURCE_CLAMP},
	{L"shared_keyedmutex", ResourceMiscFlags::SHARED_KEYEDMUTEX},
	{L"gdi_compatible", ResourceMiscFlags::GDI_COMPATIBLE},
	{L"shared_nthandle", ResourceMiscFlags::SHARED_NTHANDLE},
	{L"restricted_content", ResourceMiscFlags::RESTRICTED_CONTENT},
	{L"restrict_shared_resource", ResourceMiscFlags::RESTRICT_SHARED_RESOURCE},
	{L"restrict_shared_resource_driver", ResourceMiscFlags::RESTRICT_SHARED_RESOURCE_DRIVER},
	{L"guarded", ResourceMiscFlags::GUARDED},
	{L"tile_pool", ResourceMiscFlags::TILE_POOL},
	{L"tiled", ResourceMiscFlags::TILED},
	{L"hw_protected", ResourceMiscFlags::HW_PROTECTED},
	{NULL, ResourceMiscFlags::INVALID} // End of list marker
};

// FuzzyMatch, FilterField, FilterMatch and FilterExpr live in Filter.h.

// Forward declaration to resolve circular dependency. One of these days we
// really need to start splitting everything out of globals and making an
// effort to reduce our cyclic dependencies. Downside of this is it
// necessitates using a pointer for the textureoverride contained in the below
// struct since the definition needs to be known to statically contain one, but
// we'll hold off until post 1.3 since the FrameAnalysisOptions needs to go in
// FrameAnalysis.h to make that work, and that is an area that diverged from 1.2:
struct TextureOverride;

// A thin owner for a fuzzy [TextureOverride]. All match state lives in
// TextureOverride::filter (a FilterMatch), so this type only exists to keep the
// owning pointer used by G->mFuzzyTextureOverrides (and its sorted set) stable,
// allowing the fuzzy and [Filter<Name>] matchers to share one implementation.
class FuzzyMatchResourceDesc {
public:
	struct TextureOverride *texture_override;

	FuzzyMatchResourceDesc(std::wstring section);
	~FuzzyMatchResourceDesc();
};

// FilterMatch, FilterExpr and FilterMatches live in Filter.h.

bool TextureOverrideLess(const struct TextureOverride &lhs, const struct TextureOverride &rhs);
struct FuzzyMatchResourceDescLess {
	bool operator() (const std::shared_ptr<FuzzyMatchResourceDesc> &lhs, const std::shared_ptr<FuzzyMatchResourceDesc> &rhs) const;
};
// This set is sorted because multiple fuzzy texture overrides may match a
// given resource, but we want to make sure we always process them in the same
// order for consistent results.
typedef std::set<std::shared_ptr<FuzzyMatchResourceDesc>, FuzzyMatchResourceDescLess> FuzzyTextureOverrides;

typedef std::vector<TextureOverride*> TextureOverrideMatches;

struct TextureOverrideFuzzyMatch {
	uint32_t hash;
	TextureOverride* texture_override;
};
typedef std::vector<TextureOverrideFuzzyMatch> TextureOverrideFuzzyMatches;

TextureOverrideFuzzyMatches* get_fuzzy_matches_by_draw_info(DrawCallInfo* call_info);

template <typename DescType>
void find_texture_overrides(uint32_t hash, const DescType *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info);
void find_texture_overrides_for_resource(ID3D11Resource *resource, TextureOverrideMatches *matches, DrawCallInfo *call_info);
void find_texture_override_for_hash(uint32_t hash, TextureOverrideMatches* matches, DrawCallInfo* call_info);

void find_texture_overrides_by_hash_from_fuzzy_matches(uint32_t hash, TextureOverrideFuzzyMatches* fuzzy_matches, TextureOverrideMatches* matches, DrawCallInfo* call_info);
void find_texture_overrides_for_resource_by_hash_from_fuzzy_matches(ID3D11Resource* resource, TextureOverrideFuzzyMatches* fuzzy_matches, TextureOverrideMatches* matches, DrawCallInfo* call_info);

// Memoised lookup, see TextureOverrideCandidates. Returns NULL for resources
// without handle info, i.e. created by 3DMigoto or the swap chain, which
// never match a TextureOverride.
TextureOverrideCandidates* get_texture_override_candidates(ID3D11Resource* resource);
void find_fuzzy_texture_overrides_for_resource(ID3D11Resource* resource, TextureOverrideMatches* matches, DrawCallInfo* call_info);
void InvalidateTextureOverrideCandidates();

// Filter-index-only lookups for the texture filter operator. These find the
// effective filter_index without building the full match list, using the same
// selection order as the full finders (fuzzy matches win over exact hash
// matches - see the TODO in process_texture_filter). Callers run the fuzzy
// scan first and only fall back to the hash paths when it finds nothing.
// any_match is set if any override matched at all, so "matched but no
// filter_index" can be told apart from "no match".
const TextureOverride *find_texture_override_filter_index_for_resource_fuzzy(ID3D11Resource *resource, DrawCallInfo *call_info, bool *any_match);
const TextureOverride *find_texture_override_filter_index_for_resource_by_hash(ID3D11Resource *resource, DrawCallInfo *call_info, bool *any_match);
const TextureOverride *find_texture_override_filter_index_from_fuzzy_matches(ID3D11Resource *resource, uint32_t region_hash, TextureOverrideFuzzyMatches *fuzzy_matches, DrawCallInfo *call_info, bool *any_match);

void ClearRegionHashesGlobalCache();
