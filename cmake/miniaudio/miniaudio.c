/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* miniaudio's implementation with Ogg Vorbis decoding compiled in.
 *
 * miniaudio decodes Vorbis only when stb_vorbis's declarations come before its
 * implementation: stb_vorbis.c defines STB_VORBIS_INCLUDE_STB_VORBIS_H, and
 * miniaudio.h checks for it. Moving these includes, or building upstream's
 * miniaudio.c in place of this file, drops .ogg support without a build error. */

#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
