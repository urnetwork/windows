// The update's second signature check: the release's SHA256SUMS, signed with
// the release key, and the downloaded installer's line in it.
//
// A release may carry two assets beside its installers:
//   - SHA256SUMS: one "<64 lowercase or uppercase hex>  <asset name>" line per
//     asset, each ending in a newline (sha256sum's text format);
//   - SHA256SUMS.sig: the standard base64 of the Ed25519 signature over the
//     exact bytes of SHA256SUMS, followed by a newline.
// The elevated helper (Updater/ApplyUpdate.cpp) fetches both from the same
// release object it installs from, after GitHub's own SHA-256 of the download
// has matched, and asks CheckReleaseSignature:
//   - .sig present: the signature must verify under kReleasePublicKey, and the
//     installer's SHA-256 must equal its one line in SHA256SUMS; anything
//     else refuses (Refusal::Signature);
//   - .sig absent: refused while kRequireUpdateSignature is set, otherwise the
//     helper logs a warning and continues with the checks it already makes.
// The signing side is staged first: kRequireUpdateSignature turns on once
// every release carries the pair.
//
// The Ed25519 verification below is TweetNaCl's (public domain, Bernstein et
// al.), with SHA-512, and adds the check that S < L. Windows' CNG has Ed25519
// only on recent builds, so the helper does not depend on it.
//
// Pure, header-only and free of Windows headers: tools/update-signature-tests.cpp
// runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::update {

// Whether a release without SHA256SUMS.sig is refused. Staged: false until
// every release is signed.
inline constexpr bool kRequireUpdateSignature = false;

// The release key's Ed25519 public key, standard base64.
inline constexpr std::string_view kReleasePublicKeyBase64 = "Qm4V/UTZcuIRMFJuAzDhxf2CK6ycwxF0HFOcFhXBIYQ=";

inline constexpr std::string_view kSumsAssetName = "SHA256SUMS";
inline constexpr std::string_view kSumsSignatureAssetName = "SHA256SUMS.sig";

// The most either asset may be: a few hundred bytes each in practice.
inline constexpr std::uint64_t kMaxSumsBytes = 256 * 1024;

using PublicKey = std::array<std::uint8_t, 32>;

namespace sig_detail {

using u8 = std::uint8_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;
using gf = i64[16];

// ---- SHA-512 -------------------------------------------------------------

inline constexpr u64 kK[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

inline u64 Rot(u64 x, int c) { return (x >> c) | (x << (64 - c)); }

inline void Sha512Block(u64 h[8], const u8* block) {
  u64 w[80];
  for (int i = 0; i < 16; ++i) {
    u64 v = 0;
    for (int j = 0; j < 8; ++j) v = (v << 8) | block[8 * i + j];
    w[i] = v;
  }
  for (int i = 16; i < 80; ++i) {
    const u64 s0 = Rot(w[i - 15], 1) ^ Rot(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const u64 s1 = Rot(w[i - 2], 19) ^ Rot(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 80; ++i) {
    const u64 t1 = k + (Rot(e, 14) ^ Rot(e, 18) ^ Rot(e, 41)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const u64 t2 = (Rot(a, 28) ^ Rot(a, 34) ^ Rot(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

inline std::array<u8, 64> Sha512(const u8* data, std::size_t size) {
  u64 h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
              0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
              0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
  std::size_t offset = 0;
  for (; size - offset >= 128; offset += 128) Sha512Block(h, data + offset);
  u8 tail[256] = {};
  const std::size_t rest = size - offset;
  if (rest) std::memcpy(tail, data + offset, rest);
  tail[rest] = 0x80;
  const std::size_t blocks = rest < 112 ? 1 : 2;
  const u64 bits = static_cast<u64>(size) * 8;  // sizes here are far below 2^61
  for (int i = 0; i < 8; ++i) tail[blocks * 128 - 1 - i] = static_cast<u8>(bits >> (8 * i));
  for (std::size_t i = 0; i < blocks; ++i) Sha512Block(h, tail + 128 * i);
  std::array<u8, 64> out{};
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) out[8 * i + j] = static_cast<u8>(h[i] >> (56 - 8 * j));
  }
  return out;
}

// ---- field arithmetic mod 2^255 - 19, sixteen 16-bit limbs (TweetNaCl) ---

inline constexpr i64 kGf0[16] = {0};
inline constexpr i64 kGf1[16] = {1};
inline constexpr i64 kD[16] = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
inline constexpr i64 kD2[16] = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
inline constexpr i64 kX[16] = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
inline constexpr i64 kY[16] = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
inline constexpr i64 kI[16] = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The group order L, little-endian.
inline constexpr u8 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                              0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                              0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

inline void Set(gf r, const i64* a) {
  for (int i = 0; i < 16; ++i) r[i] = a[i];
}

inline void Carry(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (i64{1} << 16);
    const i64 c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}

inline void Select(gf p, gf q, int b) {
  const i64 c = ~(static_cast<i64>(b) - 1);
  for (int i = 0; i < 16; ++i) {
    const i64 t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

inline void Pack(u8 o[32], const gf n) {
  gf m, t;
  Set(t, n);
  Carry(t);
  Carry(t);
  Carry(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = static_cast<int>((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    Select(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = static_cast<u8>(t[i] & 0xff);
    o[2 * i + 1] = static_cast<u8>(t[i] >> 8);
  }
}

inline bool Differ(const gf a, const gf b) {
  u8 c[32], d[32];
  Pack(c, a);
  Pack(d, b);
  return std::memcmp(c, d, 32) != 0;
}

inline u8 Parity(const gf a) {
  u8 d[32];
  Pack(d, a);
  return d[0] & 1;
}

inline void Unpack(gf o, const u8* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<i64>(n[2 * i + 1]) << 8);
  o[15] &= 0x7fff;
}

inline void Add(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

inline void Sub(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

inline void Mul(gf o, const gf a, const gf b) {
  i64 t[31] = {};
  for (int i = 0; i < 16; ++i) {
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  }
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  Carry(o);
  Carry(o);
}

inline void Square(gf o, const gf a) { Mul(o, a, a); }

inline void Pow2523(gf o, const gf i) {
  gf c;
  Set(c, i);
  for (int a = 250; a >= 0; --a) {
    Square(c, c);
    if (a != 1) Mul(c, c, i);
  }
  Set(o, c);
}

inline void Invert(gf o, const gf i) {
  gf c;
  Set(c, i);
  for (int a = 253; a >= 0; --a) {
    Square(c, c);
    if (a != 2 && a != 4) Mul(c, c, i);
  }
  Set(o, c);
}

// ---- the curve, extended coordinates ---------------------------------------

inline void PointAdd(gf p[4], gf q[4]) {
  gf a, b, c, d, t, e, f, g, h;
  Sub(a, p[1], p[0]);
  Sub(t, q[1], q[0]);
  Mul(a, a, t);
  Add(b, p[0], p[1]);
  Add(t, q[0], q[1]);
  Mul(b, b, t);
  Mul(c, p[3], q[3]);
  Mul(c, c, kD2);
  Mul(d, p[2], q[2]);
  Add(d, d, d);
  Sub(e, b, a);
  Sub(f, d, c);
  Add(g, d, c);
  Add(h, b, a);
  Mul(p[0], e, f);
  Mul(p[1], h, g);
  Mul(p[2], g, f);
  Mul(p[3], e, h);
}

inline void PointSwap(gf p[4], gf q[4], int b) {
  for (int i = 0; i < 4; ++i) Select(p[i], q[i], b);
}

inline void PointPack(u8 r[32], gf p[4]) {
  gf tx, ty, zi;
  Invert(zi, p[2]);
  Mul(tx, p[0], zi);
  Mul(ty, p[1], zi);
  Pack(r, ty);
  r[31] ^= static_cast<u8>(Parity(tx) << 7);
}

inline void ScalarMult(gf p[4], gf q[4], const u8* s) {
  Set(p[0], kGf0);
  Set(p[1], kGf1);
  Set(p[2], kGf1);
  Set(p[3], kGf0);
  for (int i = 255; i >= 0; --i) {
    const int b = (s[i / 8] >> (i & 7)) & 1;
    PointSwap(p, q, b);
    PointAdd(q, p);
    PointAdd(p, p);
    PointSwap(p, q, b);
  }
}

inline void ScalarBase(gf p[4], const u8* s) {
  gf q[4];
  Set(q[0], kX);
  Set(q[1], kY);
  Set(q[2], kGf1);
  Mul(q[3], kX, kY);
  ScalarMult(p, q, s);
}

inline void ModL(u8 r[32], i64 x[64]) {
  for (int i = 63; i >= 32; --i) {
    i64 carry = 0;
    int j = i - 32;
    for (; j < i - 12; ++j) {
      x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  i64 carry = 0;
  for (int j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * kL[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (int j = 0; j < 32; ++j) x[j] -= carry * kL[j];
  for (int i = 0; i < 32; ++i) {
    x[i + 1] += x[i] >> 8;
    r[i] = static_cast<u8>(x[i] & 255);
  }
}

inline void Reduce(u8 r[32], const u8 h[64]) {
  i64 x[64];
  for (int i = 0; i < 64; ++i) x[i] = h[i];
  ModL(r, x);
}

// -A from the encoding of A; false when it is not a point.
inline bool UnpackNegative(gf r[4], const u8 p[32]) {
  gf t, chk, num, den, den2, den4, den6;
  Set(r[2], kGf1);
  Unpack(r[1], p);
  Square(num, r[1]);
  Mul(den, num, kD);
  Sub(num, num, r[2]);
  Add(den, r[2], den);
  Square(den2, den);
  Square(den4, den2);
  Mul(den6, den4, den2);
  Mul(t, den6, num);
  Mul(t, t, den);
  Pow2523(t, t);
  Mul(t, t, num);
  Mul(t, t, den);
  Mul(t, t, den);
  Mul(r[0], t, den);
  Square(chk, r[0]);
  Mul(chk, chk, den);
  if (Differ(chk, num)) Mul(r[0], r[0], kI);
  Square(chk, r[0]);
  Mul(chk, chk, den);
  if (Differ(chk, num)) return false;
  if (Parity(r[0]) == (p[31] >> 7)) Sub(r[0], kGf0, r[0]);
  Mul(r[3], r[0], r[1]);
  return true;
}

// Whether the little-endian scalar s is below L.
inline bool BelowL(const u8 s[32]) {
  for (int i = 31; i >= 0; --i) {
    if (s[i] < kL[i]) return true;
    if (s[i] > kL[i]) return false;
  }
  return false;
}

}  // namespace sig_detail

// Whether `signature` is `publicKey`'s Ed25519 signature over `message`.
inline bool VerifyEd25519(const PublicKey& publicKey, const std::array<std::uint8_t, 64>& signature,
                          std::string_view message) {
  using namespace sig_detail;
  if (!BelowL(signature.data() + 32)) return false;
  gf p[4], q[4];
  if (!UnpackNegative(q, publicKey.data())) return false;
  std::vector<u8> buffer(64 + message.size());
  std::memcpy(buffer.data(), signature.data(), 32);
  std::memcpy(buffer.data() + 32, publicKey.data(), 32);
  if (!message.empty()) std::memcpy(buffer.data() + 64, message.data(), message.size());
  const std::array<u8, 64> h = Sha512(buffer.data(), buffer.size());
  u8 k[32];
  Reduce(k, h.data());
  ScalarMult(p, q, k);
  ScalarBase(q, signature.data() + 32);
  PointAdd(p, q);
  u8 r[32];
  PointPack(r, p);
  return std::memcmp(r, signature.data(), 32) == 0;
}

// Standard base64 (with padding, no whitespace) decoded; nullopt when it is not.
inline std::optional<std::vector<std::uint8_t>> DecodeBase64(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = text[i + j];
      if (c == '=' && last && j >= 2) {
        v[j] = 0;
        ++pad;
        continue;
      }
      if (pad) return std::nullopt;  // data after '='
      v[j] = value(c);
      if (v[j] < 0) return std::nullopt;
    }
    const std::uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    if (pad < 2) out.push_back(static_cast<std::uint8_t>(n >> 8));
    if (pad < 1) out.push_back(static_cast<std::uint8_t>(n));
    // non-canonical: bits set in the padding
    if ((pad == 1 && (v[2] & 0x3)) || (pad == 2 && (v[1] & 0xf))) return std::nullopt;
  }
  return out;
}

inline std::optional<PublicKey> DecodePublicKey(std::string_view base64) {
  const auto bytes = DecodeBase64(base64);
  if (!bytes || bytes->size() != 32) return std::nullopt;
  PublicKey key{};
  std::memcpy(key.data(), bytes->data(), 32);
  return key;
}

// The release key, decoded once.
inline const PublicKey& ReleasePublicKey() {
  static const PublicKey key = DecodePublicKey(kReleasePublicKeyBase64).value_or(PublicKey{});
  return key;
}

// The SHA-256, lowercase hex, that SHA256SUMS gives `name`; nullopt when it
// has no line for it, more than one, or any line is malformed. A line is
// "<64 hex>  <name>" (two spaces, or " *" for sha256sum's binary mode) and
// every line ends in "\n".
inline std::optional<std::string> SumsDigestFor(std::string_view sums, std::string_view name) {
  if (sums.empty() || sums.back() != '\n') return std::nullopt;
  std::optional<std::string> found;
  std::size_t start = 0;
  while (start < sums.size()) {
    const std::size_t end = sums.find('\n', start);
    const std::string_view line = sums.substr(start, end - start);
    start = end + 1;
    if (line.size() < 67) return std::nullopt;
    std::string hex;
    for (std::size_t i = 0; i < 64; ++i) {
      char c = line[i];
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
      hex.push_back(c);
    }
    if (line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) return std::nullopt;
    if (line.substr(66) == name) {
      if (found) return std::nullopt;
      found = std::move(hex);
    }
  }
  return found;
}

enum class SignatureVerdict {
  // SHA256SUMS.sig verified and the installer's line matches.
  Verified,
  // No SHA256SUMS.sig, and none is required: warn and go on.
  Unsigned,
  // Refuse the update.
  Refused,
};

struct SignatureCheck {
  SignatureVerdict verdict = SignatureVerdict::Refused;
  std::string reason;
};

// The decision. `sums` and `signature` are the assets' bytes, nullopt when
// the release has no such asset; `actualSha256Hex` is the installer's
// SHA-256 as the helper hashed it.
inline SignatureCheck CheckReleaseSignature(const std::optional<std::string>& sums,
                                            const std::optional<std::string>& signature,
                                            std::string_view assetName,
                                            std::string_view actualSha256Hex, bool required,
                                            const PublicKey& publicKey) {
  if (!signature) {
    if (required) return {SignatureVerdict::Refused, "the release has no SHA256SUMS.sig"};
    return {SignatureVerdict::Unsigned,
            sums ? "the release has SHA256SUMS but no SHA256SUMS.sig"
                 : "the release has no SHA256SUMS.sig"};
  }
  if (!sums) return {SignatureVerdict::Refused, "the release has SHA256SUMS.sig but no SHA256SUMS"};
  std::string_view text = *signature;
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
  const auto bytes = DecodeBase64(text);
  if (!bytes || bytes->size() != 64) {
    return {SignatureVerdict::Refused, "SHA256SUMS.sig is not a base64 Ed25519 signature"};
  }
  std::array<std::uint8_t, 64> sig{};
  std::memcpy(sig.data(), bytes->data(), 64);
  if (!VerifyEd25519(publicKey, sig, *sums)) {
    return {SignatureVerdict::Refused, "SHA256SUMS.sig does not verify under the release key"};
  }
  const std::optional<std::string> want = SumsDigestFor(*sums, assetName);
  if (!want) {
    return {SignatureVerdict::Refused,
            "the signed SHA256SUMS has no single well-formed line for " + std::string(assetName)};
  }
  std::string actual(actualSha256Hex);
  for (char& c : actual) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  }
  if (actual != *want) {
    return {SignatureVerdict::Refused, "SHA-256 " + actual + " is not the signed SHA256SUMS's " +
                                           *want + " for " + std::string(assetName)};
  }
  return {SignatureVerdict::Verified,
          "SHA256SUMS.sig verifies and " + std::string(assetName) + " matches its line"};
}

}  // namespace urnw::update
