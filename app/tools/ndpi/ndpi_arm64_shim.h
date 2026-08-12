/*
 * ndpi_arm64_shim.h -- force-included compatibility shim for the
 * windows/arm64 nDPI cross-build proof (Phase 0 gate, task-13, FIX ROUND 3).
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
 * the ARM64 build takes the _rotr64 branch. That is nDPI's contribution to
 * this breakage.
 *
 * FIX ROUND 2 tried supplying our own `_rotr64` and got it wrong: llvm-mingw
 * ALREADY DECLARES `_rotr64` for aarch64 -- three separate headers do
 * (<stdlib.h>, <winnt.h>, <intrin.h>, per CI run 31563235934's "conflicting
 * types for '_rotr64'" at all three) -- so redeclaring it just collided
 * with mingw's own declaration. `_rotr64` was never the missing symbol.
 *
 * The ORIGINAL error (run 31562365474) named the real gap precisely: "call
 * to undeclared function '__rorq'". mingw's `_rotr64` on aarch64 exists as
 * a declaration/macro that bottoms out by calling the compiler builtin
 * `__rorq` (clang's x86-only MSVC-compatibility intrinsic backing
 * `_rotr64`), and clang simply does not provide `__rorq` as a builtin on
 * any non-x86 target. So: mingw's declaration of `_rotr64` is fine and
 * must be left alone; the missing piece one level down, the `__rorq`
 * builtin itself, is what this shim actually needs to supply. This is
 * arguably as much an llvm-mingw/clang gap (declaring an intrinsic whose
 * implementation is unavailable on the declared target) as it is an nDPI
 * bug -- nDPI's own contribution is purely the missing architecture guard
 * that routes aarch64 down this path at all; see task-13-report.md for the
 * full, corrected attribution.
 *
 * ASSUMPTION FLAGGED (not independently verified -- the llvm-mingw headers
 * are not present on the machine that wrote this shim, only CI's error
 * output is): `__rorq`'s expected signature is inferred from Microsoft's
 * documented `_rotr64(unsigned __int64, int)` prototype, which is what
 * clang's `__rorq` builtin exists to back on x86. `unsigned long long` is
 * used rather than `unsigned __int64` as a portable stand-in (bit-identical
 * on this LLP64 target, and doesn't depend on -fms-extensions being active
 * for the `__int64` spelling to exist). If this signature is still wrong,
 * the next CI run's error will say so precisely (a real conflicting-types
 * error, not a silent misbehavior), the same way run 31563235934's did for
 * `_rotr64`.
 *
 * This shim supplies __rorq() for aarch64 only, so mingw's own (untouched)
 * _rotr64 declaration has something to call.
 *
 * IMPLEMENTATION NOTE -- read before "fixing" this to be a rotate-left:
 * the function above is named binary_fuse_rotl64 (rotate LEFT) and its
 * non-Windows branch computes a genuine rotate LEFT, but its Windows
 * branch calls _rotr64 -- a rotate RIGHT. rotl(n,c) and rotr(n,c) are NOT
 * the same operation (they only coincide at c=0 and c=32 for a 64-bit
 * word), so nDPI's own Windows builds already compute a different
 * binary-fuse-filter hash than its Linux/mac builds -- a second, latent
 * upstream bug, independent of the missing-builtin one above. Our amd64
 * leg links the REAL __rorq (the actual x86 compiler builtin, via the real
 * mingw _rotr64), so for the amd64 and arm64 archives in this proof to
 * compute the same hash and actually be comparable, this shim's __rorq
 * MUST also be a rotate RIGHT. Do NOT silently correct it to rotate-left --
 * that would make this shim more "correct" than upstream while making the
 * two proof archives disagree with each other, which defeats the point of
 * proving them equivalent.
 */
#if defined(_WIN32) && defined(__aarch64__) && !defined(_MSC_VER)

/*
 * Guard against a future llvm-mingw/clang starting to provide __rorq as a
 * real builtin on aarch64 (unlikely -- it's an x86-only MSVC-compat name --
 * but round 2 just demonstrated what happens when this shim assumes a gap
 * that a toolchain update closes: a conflicting-types error, not a clean
 * no-op). __has_builtin is a clang feature-test macro, guaranteed present
 * on every clang-based toolchain this build targets (llvm-mingw is
 * clang-only), but the defined() check is kept for defensive portability
 * rather than assuming that unconditionally.
 */
#if !defined(__has_builtin) || !__has_builtin(__rorq)

/*
 * Matches the MSVC/clang _rotr64(unsigned __int64, int) prototype __rorq
 * exists to implement -- unsigned long long (not uint64_t) and int (not
 * unsigned int), because round 2's mismatch on exactly this kind of type
 * spelling is what produced "conflicting types" against mingw's own
 * declarations. Deliberately mirrors upstream's Windows rotate-RIGHT
 * behaviour (including the binary_fuse_rotl64 naming inconsistency
 * described above), not a "corrected" rotate-left, so this arm64 archive
 * matches the amd64 archive's _rotr64-based hash instead of upstream's
 * intent.
 */
static inline unsigned long long __rorq(unsigned long long value, int shift) {
  return (value >> (shift & 63)) | (value << ((64 - (shift & 63)) & 63));
}

#endif /* !__has_builtin(__rorq) */

#endif /* _WIN32 && __aarch64__ && !_MSC_VER */
