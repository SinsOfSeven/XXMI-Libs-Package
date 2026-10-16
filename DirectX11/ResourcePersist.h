#pragma once

#include <d3d11_1.h>
#include <string>

class CustomResource;
class CustomResourcePool;

// Resource persistence is independent of shader and variable persistence.
// Call with original device/context interfaces, not wrappers.
void ResourcePersistConfigure(CustomResource *resource, const std::wstring &owner,
    const std::wstring &path, bool enabled, bool debug_dump);
bool ResourcePersistRestore(CustomResource *resource, ID3D11Device *device);
void ResourcePersistObserve(CustomResource *resource);
void ResourcePersistDirty(CustomResource *resource);
void ResourcePersistDirtyPool(CustomResourcePool *pool);
void ResourcePersistRestorePool(CustomResourcePool *pool);
void ResourcePersistCheckpoint(ID3D11Device *device = nullptr);
void ResourcePersistReset();
void ResourcePersistWrite(ID3D11DeviceContext *context, ID3D11Resource *resource);
void ResourcePersistMapped(ID3D11Resource *resource, bool mapped);
void ResourcePersistWriteView(ID3D11DeviceContext *context, ID3D11View *view);
void ResourcePersistOutputs(ID3D11DeviceContext *context, bool compute);
void ResourcePersistFinish(ID3D11DeviceContext *context, ID3D11CommandList *list);
void ResourcePersistExecute(ID3D11DeviceContext *context, ID3D11CommandList *list);

struct ResourcePersistence;
