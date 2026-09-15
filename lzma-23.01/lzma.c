/* LZMA SDK (23.01) - LZMA decoder only, for ZIP method 14 (LZMA) entries.
   LZMA SDK is in the public domain (see lzma-sdk: http://www.7-zip.org/sdk.html).

   This amalgamation pulls in the LzmaDec decoder and exposes a single
   one-shot decode helper used by Archive.cpp. */

#include "LzmaDec.c"

#include <stdlib.h>

static void *LzmaAllocCallback(ISzAllocPtr p, size_t size)
{
	return malloc(size);
}

static void LzmaFreeCallback(ISzAllocPtr p, void *address)
{
	free(address);
}

static ISzAlloc g_LzmaAlloc = { LzmaAllocCallback, LzmaFreeCallback };

/* Decode a ZIP LZMA payload.
   props: 5-byte LZMA properties (property byte + 4-byte little-endian
   dictionary size), as stored after the ZIP LZMA stream header.
   Returns SZ_OK on success; *destLen is set to the decoded size. */
int LzmaDecodeZip(const unsigned char *props, size_t propsSize,
	unsigned char *dest, size_t *destLen,
	const unsigned char *src, size_t *srcLen)
{
	ELzmaStatus status;
	return LzmaDecode(dest, destLen, src, srcLen,
		props, propsSize, LZMA_FINISH_END, &status, &g_LzmaAlloc);
}