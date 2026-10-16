#pragma once

// Stub, deliberately empty.
//
// DirectX11/ImageHash.cpp includes "util.h" but uses nothing from it: the only
// function it needs is _stricmp, which comes from <windows.h> via ImageHash.h.
// The real util.h pulls in crc32c.h and the rest of the proxy's dependencies,
// which would mean building the world just to test a self-contained file.
//
// This also acts as a check: if ImageHash.cpp ever starts depending on something
// from util.h, this header will not provide it and the test build will fail with
// an undefined symbol - which is the correct outcome, because that dependency
// should be removed rather than satisfied here.
