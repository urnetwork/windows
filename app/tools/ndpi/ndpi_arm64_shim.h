/*
 * ndpi_arm64_shim.h -- force-included compatibility shim for the
 * windows/arm64 nDPI cross-build proof (Phase 0 gate, task-13, FIX ROUND 2).
 *
 * NOT part of ntop/nDPI upstream, and nDPI's own sources are never patched
 * in place -- this header is force-included ahead of everything else via
 * `-include` in Makefile.crossproof, so the pinned-tag clone stays exactly
 * what was cloned. The proof is about upstream's sources, not about a
 * local patch.
 *
 * UPSTREAM BUG (see task-13-report.md's "UPSTREAM BUGS FOUND" section for
 * the full writeup): src/lib/third_party/include/binaryfusefilter.h at
 * ntop/nDPI tag 5.0, lines 31-37:
 *
 *   static inline uint64_t binary_fuse_rotl64(uint64_t n, unsigned int c) {
 *   #ifdef _WIN32
 *       return(_rotr64(n, c));
 *   #else
 *     return (n << (c & 63)) | (n >> ((-c) & 63));
 *   #endif
 *   }
 *
 * The #ifdef is OS-only with no architecture test. _WIN32 is defined by
 * aarch64-w64-mingw32-clang too (it is a Windows target, just not x86), so
 * the ARM64 build takes the _rotr64 branch -- but _rotr64 is an x86
 * intrinsic (clang lowers it to __rorq, an x86-only builtin), which does
 * not exist on aarch64. This is why the arm64 leg failed with "call to
 * undeclared function '__rorq'": upstream has apparently never actually
 * compiled this header for Windows-on-ARM.
 *
 * This shim supplies _rotr64() for aarch64 only, so nDPI's own header
 * compiles unmodified.
 *
 * IMPLEMENTATION NOTE -- read before "fixing" this to be a rotate-left:
 * the function above is named binary_fuse_rotl64 (rotate LEFT) and its
 * non-Windows branch computes a genuine rotate LEFT, but its Windows
 * branch calls _rotr64 -- a rotate RIGHT. rotl(n,c) and rotr(n,c) are NOT
 * the same operation (they only coincide at c=0 and c=32 for a 64-bit
 * word), so nDPI's own Windows builds already compute a different
 * binary-fuse-filter hash than its Linux/mac builds -- a second, latent
 * upstream bug, independent of the missing-intrinsic one above. Our amd64
 * leg links the REAL _rotr64 (via the real Windows/mingw <intrin.h>-style
 * builtin), so for the amd64 and arm64 archives in this proof to compute
 * the same hash and actually be comparable, this shim's _rotr64 MUST also
 * be a rotate RIGHT. Do NOT silently correct it to rotate-left -- that
 * would make this shim more "correct" than upstream while making the two
 * proof archives disagree with each other, which defeats the point of
 * proving them equivalent.
 */
#if defined(_WIN32) && defined(__aarch64__) && !defined(_MSC_VER)

#include <stdint.h>

static inline uint64_t _rotr64(uint64_t n, unsigned int c) {
  /* Deliberately mirrors upstream's Windows rotate-RIGHT behaviour
   * (including the binary_fuse_rotl64 naming inconsistency described
   * above), not a "corrected" rotate-left, so this arm64 archive matches
   * the amd64 archive's _rotr64-based hash instead of upstream's intent. */
  return (n >> (c & 63)) | (n << ((64 - (c & 63)) & 63));
}

#endif /* _WIN32 && __aarch64__ && !_MSC_VER */
