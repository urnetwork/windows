#pragma once

/*
 * VENDORED for the URnetwork Windows nDPI ARM64 cross-build proof
 * (Phase 0 gate, task-13). NOT listed in the original task brief's file
 * list -- added because the brief only named ndpi_define.h, but sanity
 * -checking the build against the pinned tag's actual sources found this
 * header is unconditionally #included by src/lib/ndpi_main.c,
 * src/include/ndpi_private.h (itself included by nearly every src/lib/*.c
 * translation unit), ndpi_typedefs.h, ndpi_replace_printf.h, and directly
 * by src/lib/third_party/src/roaring.c and roaring_v2.c. Without this file
 * present at src/include/ndpi_config.h, the arm64 build fails outright on
 * almost every translation unit with "ndpi_config.h: No such file or
 * directory" -- this is not an optional refinement.
 *
 * Upstream normally generates src/include/ndpi_config.h at configure time
 * via AC_CONFIG_HEADERS (autoheader, from the many AC_DEFINE calls in
 * configure.ac) -- a step that does not run when cross-compiling with the
 * vendored Makefile.crossproof (no autotools involved).
 *
 * Content copied verbatim from ntop/nDPI tag 5.0
 * (commit 375f99ef9fb4999d778b57bbeece171b3fa9fba6), file
 * windows/src/ndpi_config.h -- the checked-in stub upstream itself ships
 * and uses for the MSVC build, which also does not run autoheader. It is
 * deliberately minimal: none of the autoheader HAVE_* feature-detection
 * macros (HAVE_PTHREAD_SETAFFINITY_NP, HAVE_MAXMINDDB, etc.) are defined,
 * which is correct for Windows -- the library code takes the "feature not
 * available" branch for all of them, exactly as the MSVC build already
 * does in production. This build does not attempt to reproduce the fuller
 * Linux/autotools feature set; it matches upstream's own proven-working
 * Windows precedent instead of guessing at one that cannot be tested here.
 */

#define NDPI_GIT_RELEASE "unknown"
