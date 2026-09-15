/* miniz (tinfl) unity build - deflate decompression only.
   miniz 3.0.2, "public domain" (see LICENSE).

   This amalgamation pulls in only the tiny inflater used for ZIP
   method 8 (deflate) entries. The deflate compressor (tdefl) and the
   miniz zip archive APIs are not compiled. */

#include "miniz_tinfl.c"