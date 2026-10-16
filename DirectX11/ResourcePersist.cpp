#include "lock.h"
#include "ResourcePersist.h"
#include "CommandList.h"
#include "BlobStore.h"
#include "globals.h"
#include "log.h"
#include "util.h"
#include "DirectXTK/Src/pch.h"
#include "DirectXTK/Src/PlatformHelpers.h"
#include "DirectXTK/Src/LoaderHelpers.h"
#include "DirectXTK/Src/DDS.h"
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
// A linked DLL build invalidates the whole disposable cache, including semantic
// changes that sizeof/layout checks alone cannot detect. No manual version bump.
uint64_t BuildIdentity()
{
    // Hash the linked binary once. Timestamp/sizeof alone can miss equal-size
    // changes and rapid rebuilds; binary contents cover runtime code and structs.
    static const uint64_t identity = []() -> uint64_t {
        wchar_t path[32768];
        if (!GetModuleFileNameW(migoto_handle, path, _countof(path))) return 0;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 0;
        uint64_t result = 14695981039346656037ull;
        uint8_t bytes[16384]; DWORD count;
        bool ok;
        while ((ok = !!ReadFile(file, bytes, sizeof(bytes), &count, nullptr)) && count)
            for (DWORD i = 0; i < count; ++i) { result ^= bytes[i]; result *= 1099511628211ull; }
        CloseHandle(file);
        return ok ? result : 0;
    }();
    return identity;
}

constexpr uint32_t max_payload = 512 * 1024 * 1024;
constexpr uint32_t max_entries = 65536;
constexpr uint64_t magic = 0x5453495352455052ull;
const GUID write_guid = {0x79f6cf82,0x8172,0x40d5,{0x97,0x72,0xf1,0x84,0x4b,0x3a,0x42,0x6a}};
const GUID list_guid = {0xcdf4af04,0x9ac2,0x4f62,{0xa3,0xcc,0x6c,0x40,0xe0,0x21,0x96,0x84}};
const GUID mapped_guid = {0x91bd3ab1,0xf705,0x48d3,{0x8a,0xbc,0x5a,0xc2,0xb8,0x62,0x79,0x11}};
std::atomic<uint64_t> generation{1};
std::atomic<bool> enabled{false};
std::atomic<UINT> output_flags{0};
std::recursive_mutex persist_mutex;

uint64_t Hash(const void *data, size_t size, uint64_t hash = 14695981039346656037ull)
{
    auto bytes = static_cast<const uint8_t *>(data);
    while (size--) { hash ^= *bytes++; hash *= 1099511628211ull; }
    return hash;
}

struct Description {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_BUFFER_DESC buffer{};
    D3D11_TEXTURE1D_DESC tex1{};
    D3D11_TEXTURE2D_DESC tex2{};
    D3D11_TEXTURE3D_DESC tex3{};
};
struct ViewDescription {
    uint32_t type = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
};
struct Record {
    uint64_t declaration = 0;
    uint64_t offset = 0, checksum = 0;
    uint32_t size = 0;
    uint32_t state = 0; // 0 uninitialized, 1 explicit null, 2 populated
    UINT stride = 0, source_stride = 0, region_offset = 0, buf_size = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    Description desc{};
    ViewDescription view{};
};
struct PoolRecord {
    uint64_t declaration = 0;
    uint32_t size = 0, index_type = 0, replacement = 0;
    std::vector<uint32_t> keys;
};
struct Header {
    uint64_t magic = ::magic, build = BuildIdentity(), checksum = 0, end = 0;
    uint32_t record_size = sizeof(Record), records = 0, pools = 0;
};
struct LiveState {
    Record record{};
    ID3D11Resource *resource = nullptr; // comparison only; never dereferenced
    uint64_t generation = 0;
    uint64_t explicit_generation = 0;
};
struct Store {
    std::wstring owner, path;
    BlobStore blob{BlobStore::ReadPolicy::Exact};
    bool loaded = false;
    bool blocked = false;
    bool leased = false;
    std::map<std::wstring, Record> records;
    std::map<std::wstring, PoolRecord> pools;
    std::map<std::wstring, LiveState> live;
};
std::map<std::wstring, std::unique_ptr<Store>> stores;

template<class T> void Put(std::vector<uint8_t> &bytes, const T &value)
{
    static_assert(std::is_trivially_copyable<T>::value, "Cache values must not own runtime state");
    auto start = reinterpret_cast<const uint8_t *>(&value);
    bytes.insert(bytes.end(), start, start + sizeof(value));
}
void PutString(std::vector<uint8_t> &bytes, const std::wstring &value)
{
    Put(bytes, uint32_t(value.size()));
    auto start = reinterpret_cast<const uint8_t *>(value.data());
    bytes.insert(bytes.end(), start, start + value.size() * sizeof(wchar_t));
}
struct Reader {
    const std::vector<uint8_t> &bytes;
    size_t pos = 0;
    template<class T> bool Get(T &value) {
        if (pos > bytes.size() || sizeof(value) > bytes.size() - pos) return false;
        memcpy(&value, bytes.data() + pos, sizeof(value)); pos += sizeof(value); return true;
    }
    bool String(std::wstring &value) {
        uint32_t length;
        if (!Get(length) || length > 32768 || length * sizeof(wchar_t) > bytes.size() - pos) return false;
        value.resize(length);
        if (length) memcpy(&value[0], bytes.data() + pos, length * sizeof(wchar_t));
        pos += length * sizeof(wchar_t); return true;
    }
};

uint64_t Declaration(const CustomResource &r)
{
    if (r.persist_declaration) return r.persist_declaration;
    // Current INI constraints, not runtime pointers or bookkeeping. BuildIdentity
    // supplies conservative compatibility when runtime definitions change.
    uint64_t h = Hash(r.filename.data(), r.filename.size() * sizeof(wchar_t));
#define FIELD(f) h = Hash(&r.f, sizeof(r.f), h)
    FIELD(override_type); FIELD(override_bind_flags); FIELD(override_misc_flags);
    FIELD(override_format); FIELD(override_color_space); FIELD(override_width);
    FIELD(override_height); FIELD(override_depth); FIELD(override_mips);
    FIELD(override_array); FIELD(override_msaa); FIELD(override_msaa_quality);
    FIELD(override_byte_width); FIELD(override_stride); FIELD(width_multiply);
    FIELD(height_multiply);
#undef FIELD
    return h;
}

uint64_t WriteGeneration(ID3D11Resource *r)
{
    uint64_t value = 0;
    UINT size = sizeof(value);
    if (r) r->GetPrivateData(write_guid, &size, &value);
    return value;
}
bool SameDevice(ID3D11Device *a, ID3D11Device *b)
{
    ComPtr<IUnknown> left, right;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&left))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&right))) && left.Get() == right.Get();
}
void Mark(ID3D11Resource *r)
{
    if (!r) return;
    UINT size = sizeof(uint64_t); uint64_t previous;
    if (FAILED(r->GetPrivateData(write_guid, &size, &previous))) return;
    uint64_t value = generation.fetch_add(1);
    r->SetPrivateData(write_guid, sizeof(value), &value);
}

// D3D owns command-list annotations and their resource references. No global
// list-pointer registry survives object destruction or pointer reuse.
class Writes : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    bool outputs_unknown = false;
    std::map<ID3D11Resource *, ComPtr<ID3D11Resource>> resources;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs; if (!n) delete this; return n;
    }
};
ComPtr<Writes> GetWrites(ID3D11DeviceChild *child, bool create)
{
    IUnknown *unknown = nullptr;
    UINT size = sizeof(unknown);
    if (SUCCEEDED(child->GetPrivateData(list_guid, &size, &unknown)) && unknown) {
        ComPtr<Writes> result; result.Attach(static_cast<Writes *>(unknown)); return result;
    }
    ComPtr<Writes> result;
    if (create) {
        result.Attach(new Writes);
        if (FAILED(child->SetPrivateDataInterface(list_guid, result.Get()))) result.Reset();
    }
    return result;
}

bool Describe(CustomResource &r, Record &record)
{
    memset(&record, 0, sizeof(record));
    record.declaration = Declaration(r);
    record.state = r.is_null ? (r.persist_null || r.resource ? 1 : 0) : r.resource ? 2 : 0;
    record.stride = r.stride; record.source_stride = r.source_stride;
    record.region_offset = r.offset; record.buf_size = r.buf_size; record.format = r.format;
    if (record.state != 2) return true;
    r.resource->GetType(&record.desc.dimension);
    switch (record.desc.dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: static_cast<ID3D11Buffer *>(r.resource)->GetDesc(&record.desc.buffer); break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: static_cast<ID3D11Texture1D *>(r.resource)->GetDesc(&record.desc.tex1); break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: static_cast<ID3D11Texture2D *>(r.resource)->GetDesc(&record.desc.tex2); break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: static_cast<ID3D11Texture3D *>(r.resource)->GetDesc(&record.desc.tex3); break;
    default: return false;
    }
    if (r.view) {
        ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11DepthStencilView> dsv;
        if (SUCCEEDED(r.view->QueryInterface(IID_PPV_ARGS(&srv)))) { record.view.type = 1; srv->GetDesc(&record.view.srv); }
        else if (SUCCEEDED(r.view->QueryInterface(IID_PPV_ARGS(&uav)))) { record.view.type = 2; uav->GetDesc(&record.view.uav); }
        else if (SUCCEEDED(r.view->QueryInterface(IID_PPV_ARGS(&rtv)))) { record.view.type = 3; rtv->GetDesc(&record.view.rtv); }
        else if (SUCCEEDED(r.view->QueryInterface(IID_PPV_ARGS(&dsv)))) { record.view.type = 4; dsv->GetDesc(&record.view.dsv); }
        else return false;
    }
    return true;
}

struct Surface { uint32_t offset, row, rows, depth; };
bool Layout(const Description &desc, std::vector<Surface> &surfaces, uint32_t &total)
{
    surfaces.clear(); total = 0;
    UINT width = 0, height = 1, depth = 1, mips = 0, arrays = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    switch (desc.dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER:
        if (!desc.buffer.ByteWidth || desc.buffer.ByteWidth > max_payload) return false;
        surfaces.push_back({0, desc.buffer.ByteWidth, 1, 1}); total = desc.buffer.ByteWidth; return true;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        width = desc.tex1.Width; mips = desc.tex1.MipLevels; arrays = desc.tex1.ArraySize; format = desc.tex1.Format; break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        if (desc.tex2.SampleDesc.Count != 1) return false;
        width = desc.tex2.Width; height = desc.tex2.Height; mips = desc.tex2.MipLevels;
        arrays = desc.tex2.ArraySize; format = desc.tex2.Format; break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        width = desc.tex3.Width; height = desc.tex3.Height; depth = desc.tex3.Depth;
        mips = desc.tex3.MipLevels; format = desc.tex3.Format; break;
    default: return false;
    }
    if (!width || !height || !depth || !mips || !arrays || width > 16384 || height > 16384 || depth > 2048
        || mips > 15 || arrays > 2048 || uint64_t(mips) * arrays > 16384) return false;
    for (UINT a = 0; a < arrays; ++a) for (UINT m = 0; m < mips; ++m) {
        size_t bytes = 0, row = 0, rows = 0;
        DirectX::LoaderHelpers::GetSurfaceInfo((std::max)(1u, width >> m),
            (std::max)(1u, height >> m), format, &bytes, &row, &rows);
        UINT slices = (std::max)(1u, depth >> m);
        if (!row || !rows || bytes != row * rows || row > max_payload || rows > max_payload
            || bytes > max_payload / slices || bytes * slices > max_payload - total) return false;
        surfaces.push_back({total, uint32_t(row), uint32_t(rows), slices});
        total += uint32_t(bytes * slices);
    }
    return total != 0;
}

HRESULT Create(ID3D11Device *device, const Description &desc, const D3D11_SUBRESOURCE_DATA *data, ID3D11Resource **out)
{
    switch (desc.dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return device->CreateBuffer(&desc.buffer, data, reinterpret_cast<ID3D11Buffer **>(out));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return device->CreateTexture1D(&desc.tex1, data, reinterpret_cast<ID3D11Texture1D **>(out));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return device->CreateTexture2D(&desc.tex2, data, reinterpret_cast<ID3D11Texture2D **>(out));
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return device->CreateTexture3D(&desc.tex3, data, reinterpret_cast<ID3D11Texture3D **>(out));
    default: return E_INVALIDARG;
    }
}

bool Capture(CustomResource &r, const Record &record, std::vector<uint8_t> &bytes)
{
    UINT mapped = 0, mapped_size = sizeof(mapped);
    r.resource->GetPrivateData(mapped_guid, &mapped_size, &mapped);
    if (mapped) return false;
    std::vector<Surface> surfaces;
    uint32_t total;
    if (!Layout(record.desc, surfaces, total)) return false;
    // Hidden append/consume counters are not resource bytes.
    if (record.view.type == 2 && (record.view.uav.Buffer.Flags &
        (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) &&
        record.desc.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    Description staging = record.desc;
#define STAGING(d) d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0
    switch (staging.dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: STAGING(staging.buffer); staging.buffer.StructureByteStride = 0; break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: STAGING(staging.tex1); break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: STAGING(staging.tex2); break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: STAGING(staging.tex3); break;
    default: return false;
    }
#undef STAGING
    ComPtr<ID3D11Device> device; r.resource->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    ComPtr<ID3D11Resource> copy;
    if (FAILED(Create(device.Get(), staging, nullptr, &copy))) return false;
    context->CopyResource(copy.Get(), r.resource);
    bytes.resize(total);
    for (UINT i = 0; i < surfaces.size(); ++i) {
        auto &s = surfaces[i]; D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(copy.Get(), i, D3D11_MAP_READ, 0, &mapped))) return false;
        bool valid = mapped.pData && (staging.dimension == D3D11_RESOURCE_DIMENSION_BUFFER ||
            (mapped.RowPitch >= s.row && (s.depth == 1 || uint64_t(mapped.DepthPitch) >= uint64_t(mapped.RowPitch) * s.rows)));
        if (valid) for (UINT z = 0; z < s.depth; ++z) for (UINT y = 0; y < s.rows; ++y)
            memcpy(bytes.data() + s.offset + (size_t(z) * s.rows + y) * s.row,
                static_cast<const uint8_t *>(mapped.pData) + size_t(z) * mapped.DepthPitch + size_t(y) * mapped.RowPitch, s.row);
        context->Unmap(copy.Get(), i);
        if (!valid) return false;
    }
    return true;
}

void Load(Store &store)
{
    if (store.loaded) return;
    store.loaded = true;
    store.blob.set_path((store.path + L".blob").c_str());
    if (GetFileAttributesW((store.path + L".blob").c_str()) != INVALID_FILE_ATTRIBUTES && !store.blob.lock_exclusive(false)) {
        store.blocked = true;
        LogWarningW(L"ResourcePersist: cache is in use/unwritable: %ls\n", store.path.c_str()); return;
    }
    store.leased = store.blob.is_open() && !store.blocked;
    std::vector<uint8_t> bytes;
    if (!blob_store_metadata_read((store.path + L".dat").c_str(), &bytes)) return;
    Reader reader{bytes}; Header header;
    std::wstring owner;
    if (!reader.Get(header) || header.magic != magic || !header.build || header.build != BuildIdentity() || header.record_size != sizeof(Record)
        || header.records > max_entries || header.pools > max_entries || !store.blob.validate(header.end)
        || header.checksum != Hash(bytes.data() + sizeof(Header), bytes.size() - sizeof(Header))
        || !reader.String(owner) || owner != store.owner) {
        LogInfoW(L"ResourcePersist: ignoring incompatible/invalid cache %ls.dat\n", store.path.c_str()); return;
    }
    std::map<std::wstring, Record> records;
    std::map<std::wstring, PoolRecord> pools;
    bool valid = true;
    for (UINT i = 0; valid && i < header.records; ++i) {
        std::wstring name; Record record;
        valid = reader.String(name) && reader.Get(record) && record.state <= 2 && record.view.type <= 4;
        if (valid && record.state == 2) {
            std::vector<Surface> layout; uint32_t total;
            valid = Layout(record.desc, layout, total) && total == record.size &&
                blob_store_range_valid(record.offset, record.size, header.end);
            if (valid && record.desc.dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
                valid = record.region_offset <= record.desc.buffer.ByteWidth &&
                    record.buf_size <= record.desc.buffer.ByteWidth - record.region_offset;
        } else if (valid) valid = record.size == 0;
        if (valid) valid = records.emplace(name, record).second;
    }
    for (UINT i = 0; valid && i < header.pools; ++i) {
        std::wstring name; PoolRecord pool;
        valid = reader.String(name) && reader.Get(pool.declaration) && reader.Get(pool.size) &&
            reader.Get(pool.index_type) && reader.Get(pool.replacement) && pool.size && pool.size <= max_entries && pool.replacement < pool.size;
        uint32_t count = 0;
        valid = valid && reader.Get(count) && (count == 0 || count == pool.size);
        if (!valid) break;
        pool.keys.resize(count); std::set<uint32_t> keys;
        for (auto &key : pool.keys) {
            if (!reader.Get(key) || (key != UINT32_MAX && !keys.insert(key).second)) { valid = false; break; }
        }
        if (valid) valid = pools.emplace(name, std::move(pool)).second;
    }
    if (!valid || reader.pos != bytes.size()) {
        LogWarningW(L"ResourcePersist: ignoring corrupt cache %ls.dat\n", store.path.c_str()); return;
    }
    store.records.swap(records); store.pools.swap(pools); store.blob.set_logical_end(header.end);
}

Store &GetStore(const CustomResource &r)
{
    auto &ptr = stores[r.persist_path];
    if (!ptr) { ptr.reset(new Store); ptr->owner = r.persist_owner; ptr->path = r.persist_path; }
    Load(*ptr); return *ptr;
}

bool LockStore(Store &store)
{
    if (store.blocked) return false;
    if (store.leased) return true;
    if (!store.blob.lock_exclusive(true)) {
        store.blocked = true;
        LogWarningW(L"ResourcePersist: cannot lock owner %ls (another process or unwritable directory)\n", store.owner.c_str());
        return false;
    }
    // A store first seen before its files existed may have been written by
    // another process meanwhile. Reload its published offsets under the lease.
    store.leased = true;
    store.loaded = false;
    Load(store);
    return true;
}

std::wstring SafeName(const std::wstring &name)
{
    std::wstring result;
    for (wchar_t c : name) result += (iswalnum(c) || c == L'_' || c == L'-') ? c : L'_';
    if (result.size() > 80) result.resize(80);
    // Include full identity so sanitization and truncation cannot alias exports.
    wchar_t suffix[24]; swprintf_s(suffix, L"_%016llx", Hash(name.data(), name.size() * sizeof(wchar_t)));
    return result + suffix;
}

void Dump(Store &store, const std::wstring &name, const Record &record, const std::vector<uint8_t> &bytes)
{
    std::wstring root = store.path + L"_" + SafeName(name);
    if (record.state != 2) { DeleteFileW((root + L".buf").c_str()); DeleteFileW((root + L".dds").c_str()); return; }
    std::vector<uint8_t> data;
    bool buffer = record.desc.dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
    if (!buffer) {
        DirectX::DDS_HEADER header{}; DirectX::DDS_HEADER_DXT10 ext{};
        header.size = sizeof(header); header.flags = DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_MIPMAP;
        header.ddspf = DirectX::DDSPF_DX10; header.caps = DDS_SURFACE_FLAGS_TEXTURE | DDS_SURFACE_FLAGS_MIPMAP;
        ext.resourceDimension = record.desc.dimension; ext.arraySize = 1;
        if (record.desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
            header.width = record.desc.tex1.Width; header.height = 1; header.mipMapCount = record.desc.tex1.MipLevels;
            ext.dxgiFormat = record.desc.tex1.Format; ext.arraySize = record.desc.tex1.ArraySize;
        } else if (record.desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            header.width = record.desc.tex2.Width; header.height = record.desc.tex2.Height; header.mipMapCount = record.desc.tex2.MipLevels;
            ext.dxgiFormat = record.desc.tex2.Format; ext.arraySize = record.desc.tex2.ArraySize;
            if (record.desc.tex2.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
                ext.miscFlag = D3D11_RESOURCE_MISC_TEXTURECUBE; ext.arraySize /= 6;
                header.caps2 = DDS_CUBEMAP_ALLFACES;
            }
        } else {
            header.width = record.desc.tex3.Width; header.height = record.desc.tex3.Height;
            header.depth = record.desc.tex3.Depth; header.mipMapCount = record.desc.tex3.MipLevels;
            header.flags |= DDS_HEADER_FLAGS_VOLUME; header.caps2 = DDS_FLAGS_VOLUME;
            ext.dxgiFormat = record.desc.tex3.Format;
        }
        Put(data, uint32_t(DirectX::DDS_MAGIC)); Put(data, header); Put(data, ext);
    }
    data.insert(data.end(), bytes.begin(), bytes.end());
    std::wstring path = root + (buffer ? L".buf" : L".dds");
    if (!blob_store_atomic_write(path.c_str(), data.data(), data.size()))
        LogWarningW(L"ResourcePersist: debug export failed %ls\n", path.c_str());
    else DeleteFileW((root + (buffer ? L".dds" : L".buf")).c_str());
}
} // namespace

// This friend reads/writes pool addressing metadata, never variable values or
// variable persistence flags. It does not use SwitchElementType during restore.
struct ResourcePersistence {
    static bool CompatiblePool(CustomResourcePool &p, Store &store) {
        auto found = store.pools.find(p.name);
        return !p.source_pool && found != store.pools.end() &&
            found->second.size == p.pool_size && found->second.index_type == uint32_t(p.index_type) &&
            found->second.declaration == Declaration(*p.resource_template);
    }
    static void DirtyPool(CustomResourcePool &p) {
        for (auto &element : p.elements) if (element.resource) ResourcePersistDirty(element.resource);
    }
    static PoolRecord DescribePool(CustomResourcePool &p) {
        PoolRecord result;
        result.declaration = Declaration(*p.resource_template);
        result.size = uint32_t(p.pool_size); result.index_type = uint32_t(p.index_type);
        result.replacement = uint32_t(p.last_replacement_index);
        if (p.index_type & PoolIndexType::INDEX_TABLE_MASK)
            for (auto &slot : p.index_table) result.keys.push_back(slot.key);
        return result;
    }
    static bool SamePool(const PoolRecord &a, const PoolRecord &b) {
        return a.declaration == b.declaration && a.size == b.size && a.index_type == b.index_type &&
            a.replacement == b.replacement && a.keys == b.keys;
    }
    static void RestorePool(CustomResourcePool &p) {
        if (!p.resource_template || !p.resource_template->persist || p.source_pool) return;
        Store &store = GetStore(*p.resource_template);
        if (store.owner != p.resource_template->persist_owner) return;
        auto found = store.pools.find(p.name);
        if (found == store.pools.end()) return;
        auto &saved = found->second;
        auto current = DescribePool(p);
        if (saved.declaration != current.declaration || saved.size != current.size || saved.index_type != current.index_type) return;
        if (p.index_type & PoolIndexType::INDEX_TABLE_MASK) {
            if (saved.keys.size() != p.pool_size) return;
            p.index_map->clear();
            for (size_t i = 0; i < saved.keys.size(); ++i) {
                p.index_table[i].key = saved.keys[i];
                p.index_table[i].last_update_frame = G->frame_no;
                if (saved.keys[i] != UINT32_MAX) p.index_map->insert(saved.keys[i], i);
            }
        }
        p.last_replacement_index = saved.replacement;
        for (size_t i = 0; i < p.pool_size; ++i) {
            auto found_resource = store.records.find(p.name + L"_" + std::to_wstring(i));
            if (found_resource == store.records.end() || !found_resource->second.state ||
                found_resource->second.declaration != current.declaration) continue;
            p.InitializeResource(i);
            auto &element = p.elements[i];
            element.type = element.variable ? PoolElement::Type::Mixed : PoolElement::Type::Resource;
            if (!p.index_table.empty()) p.index_table[i].last_update_frame = G->frame_no;
        }
    }
    static void Checkpoint(ID3D11Device *only_device);
};

void ResourcePersistConfigure(CustomResource *r, const std::wstring &owner,
    const std::wstring &path, bool persist, bool debug)
{
    r->persist = persist; r->debug_dump = debug; r->persist_owner = owner; r->persist_path = path;
    if (persist && !r->persist_declaration) r->persist_declaration = Declaration(*r);
    if (persist) enabled = true;
}

static bool RestoreResource(CustomResource *r, ID3D11Device *device)
{
    if (!r->persist || r->persist_checked || r->pool_index == -1) return false;
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    r->persist_checked = true;
    Store &store = GetStore(*r);
    if (store.owner != r->persist_owner) return false;
    if (r->pool && !ResourcePersistence::CompatiblePool(*r->pool, store)) return false;
    auto found = store.records.find(r->name);
    if (found == store.records.end()) return false;
    Record saved = found->second;
    if (!saved.state || saved.declaration != Declaration(*r)) return false;
    if (saved.state == 1) {
        r->is_null = true; r->substantiated = true; r->persist_null = true;
        r->stride = saved.stride; r->source_stride = saved.source_stride;
        r->offset = saved.region_offset; r->buf_size = saved.buf_size; r->format = saved.format;
        store.live[r->name] = {saved, nullptr, 0}; return true;
    }
    std::vector<Surface> surfaces; uint32_t total;
    if (!Layout(saved.desc, surfaces, total) || total != saved.size) return false;
    std::vector<uint8_t> bytes(total);
    if (!store.blob.read(saved.offset, saved.size, bytes.data()) || Hash(bytes.data(), bytes.size()) != saved.checksum) {
        LogWarningW(L"ResourcePersist: payload invalid for %ls\n", r->name.c_str()); return false;
    }
    Description desc = saved.desc;
    // Current usage requirements must still be supported by the cached object.
    UINT stored_bind = desc.dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? desc.buffer.BindFlags :
        desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? desc.tex1.BindFlags :
        desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? desc.tex2.BindFlags : desc.tex3.BindFlags;
    UINT stored_misc = desc.dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? desc.buffer.MiscFlags :
        desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? desc.tex1.MiscFlags :
        desc.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? desc.tex2.MiscFlags : desc.tex3.MiscFlags;
    if ((stored_bind & r->bind_flags) != UINT(r->bind_flags) ||
        (stored_misc & r->misc_flags) != UINT(r->misc_flags)) return false;
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    for (auto &s : surfaces) initial.push_back({bytes.data() + s.offset, s.row, s.row * s.rows});
    ComPtr<ID3D11Resource> resource;
    if (FAILED(Create(device, desc, initial.data(), &resource))) {
        LogWarningW(L"ResourcePersist: could not restore %ls; using INI initialization\n", r->name.c_str()); return false;
    }
    ComPtr<ID3D11View> view; HRESULT hr = S_OK;
    switch (saved.view.type) {
    case 1: { ComPtr<ID3D11ShaderResourceView> v; hr = device->CreateShaderResourceView(resource.Get(), &saved.view.srv, &v); view = v; break; }
    case 2: { ComPtr<ID3D11UnorderedAccessView> v; hr = device->CreateUnorderedAccessView(resource.Get(), &saved.view.uav, &v); view = v; break; }
    case 3: { ComPtr<ID3D11RenderTargetView> v; hr = device->CreateRenderTargetView(resource.Get(), &saved.view.rtv, &v); view = v; break; }
    case 4: { ComPtr<ID3D11DepthStencilView> v; hr = device->CreateDepthStencilView(resource.Get(), &saved.view.dsv, &v); view = v; break; }
    }
    if (FAILED(hr)) return false;
    r->resource = resource.Detach(); r->view = view.Detach(); r->device = device;
    r->stride = saved.stride; r->source_stride = saved.source_stride;
    r->offset = saved.region_offset; r->buf_size = saved.buf_size; r->format = saved.format;
    r->is_null = false; r->substantiated = true;
    // Rebuild cached metadata on demand for the newly restored resource.
    r->handle_info.reset();
    ResourcePersistObserve(r);
    store.live[r->name] = {saved, r->resource, WriteGeneration(r->resource)};
    LogInfoW(L"ResourcePersist: restored %ls (%u bytes)\n", r->name.c_str(), saved.size);
    return true;
}

bool ResourcePersistRestore(CustomResource *r, ID3D11Device *device)
{
    try { return RestoreResource(r, device); }
    catch (const std::exception &e) {
        LogWarning("ResourcePersist: restore failed: %s\n", e.what());
        return r->resource && !r->is_null;
    }
}

void ResourcePersistRestorePool(CustomResourcePool *p)
{
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    try { ResourcePersistence::RestorePool(*p); }
    catch (const std::exception &e) { LogWarning("ResourcePersist: pool restore failed: %s\n", e.what()); }
}

void ResourcePersistence::Checkpoint(ID3D11Device *only_device)
{
    struct Update { CustomResource *resource; Record record; uint64_t generation; std::vector<uint8_t> bytes; ID3D11Resource *captured; uint64_t explicit_generation; };
    std::map<Store *, std::vector<Update>> updates;
    std::set<Store *> failed;
    std::map<Store *, std::map<std::wstring, PoolRecord>> pool_updates;
    for (auto &pair : customResources) {
        auto &r = pair.second;
        if (!r.persist || r.pool_index == -1 || (!r.substantiated && !r.resource && !r.persist_checked)) continue;
        if (r.pool && r.pool->source_pool) {
            LogWarningW(L"ResourcePersist: reference pool persistence unsupported: %ls\n", r.pool->name.c_str()); continue;
        }
        if (only_device && r.resource) {
            ComPtr<ID3D11Device> device; r.resource->GetDevice(&device);
            if (!SameDevice(device.Get(), only_device)) continue;
        }
        Store &store = GetStore(r);
        if (store.blocked) continue;
        if (store.owner != r.persist_owner) { LogWarningW(L"ResourcePersist: owner filename collision: %ls\n", r.persist_path.c_str()); continue; }
        Record record;
        if (!Describe(r, record) || !record.state) continue;
        uint64_t gen = WriteGeneration(r.resource);
        auto live = store.live.find(r.name);
        Record comparison = record;
        if (live != store.live.end()) {
            comparison.offset = live->second.record.offset; comparison.size = live->second.record.size;
            comparison.checksum = live->second.record.checksum;
            if (live->second.resource == (r.is_null ? nullptr : r.resource) && live->second.generation == gen &&
                live->second.explicit_generation == r.persist_dirty_generation &&
                !memcmp(&comparison, &live->second.record, sizeof(Record))) continue;
        }
        Update update{&r, record, gen, {}, record.state == 2 ? r.resource : nullptr, r.persist_dirty_generation};
        if (record.state == 2 && !Capture(r, record, update.bytes)) {
            failed.insert(&store);
            LogWarningW(L"ResourcePersist: capture failed/unsupported: %ls; previous owner snapshot retained\n", r.name.c_str()); continue;
        }
        updates[&store].push_back(std::move(update));
    }
    for (auto &pair : customResourcePools) {
        auto &p = pair.second;
        if (!p.resource_template || !p.resource_template->persist || p.source_pool) continue;
        // A variable-only pool never creates a resource store.
        bool has_resources = false;
        for (auto &element : p.elements) if (element.resource && (element.resource->resource || element.resource->substantiated || element.resource->persist_checked)) has_resources = true;
        if (!has_resources) continue;
        Store &store = GetStore(*p.resource_template);
        if (store.owner != p.resource_template->persist_owner) continue;
        auto record = DescribePool(p);
        auto old = store.pools.find(p.name);
        if (old == store.pools.end() || !SamePool(old->second, record)) pool_updates[&store][p.name] = record;
        updates[&store];
    }
    for (auto &pair : updates) {
        Store &store = *pair.first;
        if (failed.count(&store)) continue;
        if (pair.second.empty() && pool_updates[&store].empty()) continue;
        if (!LockStore(store)) continue;
        auto records = store.records; auto pools = store.pools;
        bool success = true;
        for (auto &u : pair.second) {
            if (!u.bytes.empty()) {
                u.record.size = uint32_t(u.bytes.size()); u.record.checksum = Hash(u.bytes.data(), u.bytes.size());
                if (!store.blob.append(u.bytes.data(), u.bytes.size(), &u.record.offset)) { success = false; break; }
            }
            records[u.resource->name] = u.record;
        }
        if (!success || !store.blob.flush()) { LogWarning("ResourcePersist: blob publication failed\n"); continue; }
        for (auto &p : pool_updates[&store]) pools[p.first] = p.second;
        Header header; header.end = store.blob.logical_end(); header.records = uint32_t(records.size()); header.pools = uint32_t(pools.size());
        std::vector<uint8_t> bytes; Put(bytes, header); PutString(bytes, store.owner);
        for (auto &r : records) { PutString(bytes, r.first); Put(bytes, r.second); }
        for (auto &p : pools) {
            PutString(bytes, p.first); Put(bytes, p.second.declaration); Put(bytes, p.second.size);
            Put(bytes, p.second.index_type); Put(bytes, p.second.replacement); Put(bytes, uint32_t(p.second.keys.size()));
            for (auto key : p.second.keys) Put(bytes, key);
        }
        header.checksum = Hash(bytes.data() + sizeof(Header), bytes.size() - sizeof(Header));
        memcpy(bytes.data(), &header, sizeof(header));
        if (bytes.size() > BLOB_STORE_METADATA_MAX || !blob_store_metadata_write((store.path + L".dat").c_str(), bytes.data(), bytes.size())) {
            LogWarningW(L"ResourcePersist: metadata publication failed: %ls\n", store.path.c_str()); continue;
        }
        store.records.swap(records); store.pools.swap(pools);
        for (auto &u : pair.second) {
            store.live[u.resource->name] = {u.record, u.captured, u.generation, u.explicit_generation};
            if (u.resource->debug_dump) Dump(store, u.resource->name, u.record, u.bytes);
        }
        LogInfoW(L"ResourcePersist: saved %zu changed resources for %ls\n", pair.second.size(), store.owner.c_str());
    }
}

void ResourcePersistCheckpoint(ID3D11Device *device)
{
    if (!enabled) return;
    // Registry -> store is the lock order. No shader store mutex is used.
    CriticalSectionGuard(&G->mCriticalSection);
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    try { ResourcePersistence::Checkpoint(device); }
    catch (const std::exception &e) { LogWarning("ResourcePersist: checkpoint failed: %s\n", e.what()); }
}
void ResourcePersistReset()
{
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    stores.clear(); enabled = false;
}
void ResourcePersistObserve(CustomResource *r)
{
    if (!r->persist || !r->resource) return;
    UINT size = sizeof(uint64_t); uint64_t value;
    if (FAILED(r->resource->GetPrivateData(write_guid, &size, &value))) {
        value = generation.fetch_add(1);
        r->resource->SetPrivateData(write_guid, sizeof(value), &value);
        D3D11_RESOURCE_DIMENSION dimension; UINT flags = 0;
        r->resource->GetType(&dimension);
        switch (dimension) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: { D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer *>(r->resource)->GetDesc(&d); flags = d.BindFlags; break; }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: { D3D11_TEXTURE1D_DESC d; static_cast<ID3D11Texture1D *>(r->resource)->GetDesc(&d); flags = d.BindFlags; break; }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: { D3D11_TEXTURE2D_DESC d; static_cast<ID3D11Texture2D *>(r->resource)->GetDesc(&d); flags = d.BindFlags; break; }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: { D3D11_TEXTURE3D_DESC d; static_cast<ID3D11Texture3D *>(r->resource)->GetDesc(&d); flags = d.BindFlags; break; }
        }
        output_flags.fetch_or(flags);
    }
}
void ResourcePersistDirty(CustomResource *r)
{
    if (!r || !r->persist) {
        LogWarning("ResourcePersist: persist_dirty target does not have persist enabled\n"); return;
    }
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    r->persist_dirty_generation = generation.fetch_add(1);
    if (r->resource && !r->is_null) { ResourcePersistObserve(r); Mark(r->resource); }
}
void ResourcePersistDirtyPool(CustomResourcePool *p)
{
    if (!p || !p->resource_template || !p->resource_template->persist) {
        LogWarning("ResourcePersist: persist_dirty pool does not have persist enabled\n"); return;
    }
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    ResourcePersistence::DirtyPool(*p);
}
void ResourcePersistWrite(ID3D11DeviceContext *context, ID3D11Resource *resource)
{
    if (!enabled || !resource) return;
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        auto writes = GetWrites(context, true);
        if (writes) writes->resources[resource] = resource;
    } else Mark(resource);
}
void ResourcePersistMapped(ID3D11Resource *resource, bool mapped)
{
    if (!enabled || !resource || !WriteGeneration(resource)) return;
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    UINT count = 0, size = sizeof(count);
    resource->GetPrivateData(mapped_guid, &size, &count);
    if (mapped) ++count;
    else if (count) --count;
    resource->SetPrivateData(mapped_guid, sizeof(count), &count);
}
void ResourcePersistWriteView(ID3D11DeviceContext *context, ID3D11View *view)
{
    if (!enabled || !view) return;
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource); ResourcePersistWrite(context, resource.Get());
}
void ResourcePersistOutputs(ID3D11DeviceContext *context, bool compute)
{
    if (!enabled) return;
    UINT flags = output_flags.load();
    if (compute ? !(flags & D3D11_BIND_UNORDERED_ACCESS) :
        !(flags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_STREAM_OUTPUT))) return;
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
        // Deferred contexts do not support Get* binding queries. Conservatively
        // dirty device-owned persisted resources only when this list executes.
        std::lock_guard<std::recursive_mutex> guard(persist_mutex);
        auto writes = GetWrites(context, true);
        if (writes) writes->outputs_unknown = true;
        return;
    }
    ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT]{};
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    UINT slots = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    if (compute) context->CSGetUnorderedAccessViews(0, slots, uavs);
    else {
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView *dsv = nullptr;
        if (flags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        for (auto view : rtvs) if (view) { ResourcePersistWriteView(context, view); view->Release(); }
        if (dsv) { ResourcePersistWriteView(context, dsv); dsv->Release(); }
        if (flags & D3D11_BIND_UNORDERED_ACCESS)
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, slots, uavs);
        ID3D11Buffer *buffers[D3D11_SO_STREAM_COUNT]{};
        if (flags & D3D11_BIND_STREAM_OUTPUT) context->SOGetTargets(D3D11_SO_STREAM_COUNT, buffers);
        for (auto buffer : buffers) if (buffer) { ResourcePersistWrite(context, buffer); buffer->Release(); }
    }
    for (auto view : uavs) if (view) { ResourcePersistWriteView(context, view); view->Release(); }
}
void ResourcePersistFinish(ID3D11DeviceContext *context, ID3D11CommandList *list)
{
    if (!enabled || !list) return;
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    auto writes = GetWrites(context, false);
    if (writes) list->SetPrivateDataInterface(list_guid, writes.Get());
    context->SetPrivateDataInterface(list_guid, nullptr);
}
void ResourcePersistExecute(ID3D11DeviceContext *context, ID3D11CommandList *list)
{
    if (!enabled || !list) return;
    CriticalSectionGuard(&G->mCriticalSection);
    std::lock_guard<std::recursive_mutex> guard(persist_mutex);
    auto writes = GetWrites(list, false);
    if (writes) for (auto &r : writes->resources) Mark(r.first);
    if (writes && writes->outputs_unknown) {
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        for (auto &pair : customResources) {
            auto &r = pair.second;
            if (!r.persist || !r.resource) continue;
            ComPtr<ID3D11Device> owner; r.resource->GetDevice(&owner);
            if (SameDevice(owner.Get(), device.Get())) Mark(r.resource);
        }
    }
}
