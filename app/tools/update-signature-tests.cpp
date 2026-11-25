// Executable spec for the update's second signature check
// (Common/UpdateSignature.h): SHA-512 and Ed25519 against known answers, the
// SHA256SUMS line format, and what CheckReleaseSignature decides for each
// shape of release, with a test key injected in place of the release key.
// The test key is the one Go's crypto/ed25519 derives from the seed
// 01 02 .. 20, and its signature below is Go's over kSums.
//
//   c++ -std=c++20 -I ../src/Common update-signature-tests.cpp -o /tmp/update-signature-tests
//   /tmp/update-signature-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "UpdateSignature.h"

using namespace urnw::update;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

std::string Hex(const std::uint8_t* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 15]);
  }
  return out;
}

template <std::size_t N>
std::array<std::uint8_t, N> FromHex(std::string_view hex) {
  std::array<std::uint8_t, N> out{};
  auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < N; ++i) out[i] = static_cast<std::uint8_t>(v(hex[2 * i]) * 16 + v(hex[2 * i + 1]));
  return out;
}

std::string FromHexString(std::string_view hex) {
  std::string out;
  auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(v(hex[i]) * 16 + v(hex[i + 1])));
  return out;
}

constexpr std::string_view kAsset = "URnetwork-2026.11.25-x64.msi";
constexpr std::string_view kAssetSha = "1483b22fc51e1d3556f0622b4e9de2ce65a546f482b54d6d67cbacf181cc03a5";
const std::string kSums =
    "1483b22fc51e1d3556f0622b4e9de2ce65a546f482b54d6d67cbacf181cc03a5  URnetwork-2026.11.25-x64.msi\n"
    "0000000000000000000000000000000000000000000000000000000000000001  URnetwork-2026.11.25-arm64.msi\n";
const std::string kSig =
    "GcKfyipzcOsbGLq/SYu8q+/oSA47RNGzOAmmg3BvcASMsl9Pm977Wl9Vt9YjyXtrW4jm82gbC5qccX36DkWXDQ==\n";
const PublicKey kTestKey = *DecodePublicKey("ebVWLo/mVPlAeLES6KmLp5AfhTrmlb7X4OORC60ElmQ=");
const PublicKey kOtherKey = *DecodePublicKey("J07WsmgF0v6NBgUp7povKHY7aXbZxf0d7jj/NrO3aTk=");

void Sha512KnownAnswers() {
  const auto abc = sig_detail::Sha512(reinterpret_cast<const std::uint8_t*>("abc"), 3);
  Check(Hex(abc.data(), 64) ==
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "SHA-512 of abc");
  std::uint8_t bytes[200];
  for (int i = 0; i < 200; ++i) bytes[i] = static_cast<std::uint8_t>(i);
  const auto two = sig_detail::Sha512(bytes, 200);
  Check(Hex(two.data(), 64) ==
            "986058e9895e2c2ab8f9e8cbdf801db12a44842a56a91d5a4e87b1fc98b29372"
            "2c4664142e42c3c551ff898646268cd92b84ed230b8c94bed7798d4f27cd7465",
        "SHA-512 of 200 bytes, two blocks");
}

void Rfc8032() {
  const auto pk1 = FromHex<32>("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  const auto sig1 = FromHex<64>(
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  Check(VerifyEd25519(pk1, sig1, ""), "RFC 8032 test 1 verifies");
  Check(!VerifyEd25519(pk1, sig1, "x"), "RFC 8032 test 1 on another message does not");
  const auto pk2 = FromHex<32>("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
  const auto sig2 = FromHex<64>(
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
  Check(VerifyEd25519(pk2, sig2, FromHexString("72")), "RFC 8032 test 2 verifies");
  Check(!VerifyEd25519(pk1, sig2, FromHexString("72")), "test 2's signature under test 1's key does not");
  auto flipped = sig2;
  flipped[10] ^= 1;
  Check(!VerifyEd25519(pk2, flipped, FromHexString("72")), "a flipped R bit does not");
  // S + L: the same signature, malleated; refused by the S < L check.
  auto malleated = sig2;
  int carry = 0;
  for (int i = 0; i < 32; ++i) {
    const int sum = malleated[32 + i] + sig_detail::kL[i] + carry;
    malleated[32 + i] = static_cast<std::uint8_t>(sum);
    carry = sum >> 8;
  }
  Check(!VerifyEd25519(pk2, malleated, FromHexString("72")), "S + L is refused");
}

void Base64() {
  Check(DecodeBase64("") && DecodeBase64("")->empty(), "empty base64");
  Check(DecodeBase64("TWFu") == std::vector<std::uint8_t>{'M', 'a', 'n'}, "Man");
  Check(DecodeBase64("TWE=") == std::vector<std::uint8_t>{'M', 'a'}, "Ma");
  Check(DecodeBase64("TQ==") == std::vector<std::uint8_t>{'M'}, "M");
  Check(!DecodeBase64("TWE"), "unpadded is refused");
  Check(!DecodeBase64("TR=="), "bits in the padding are refused");
  Check(!DecodeBase64("T=E="), "data after = is refused");
  Check(!DecodeBase64("TW-u"), "url-safe alphabet is refused");
  Check(DecodePublicKey(kReleasePublicKeyBase64).has_value(), "the embedded release key decodes to 32 bytes");
  Check(ReleasePublicKey() != PublicKey{}, "the release key is not all zero");
}

void Sums() {
  Check(SumsDigestFor(kSums, kAsset) == std::string(kAssetSha), "the installer's line");
  Check(!SumsDigestFor(kSums, "URnetwork-2026.11.25-x86.msi"), "no line for another name");
  Check(!SumsDigestFor(kSums, "URnetwork-2026.11.25-x64.ms"), "a prefix of a name is not it");
  Check(!SumsDigestFor(kSums.substr(0, kSums.size() - 1), kAsset), "a last line without a newline");
  Check(!SumsDigestFor(kSums + kSums, kAsset), "two lines for the name are refused");
  Check(!SumsDigestFor(std::string(kAssetSha) + " " + std::string(kAsset) + "\n", kAsset), "one space");
  Check(!SumsDigestFor("zz" + kSums.substr(2), kAsset), "non-hex digits");
  Check(!SumsDigestFor(kSums + "\n", kAsset), "an empty line");
  Check(SumsDigestFor(std::string(kAssetSha) + " *" + std::string(kAsset) + "\n", kAsset) ==
            std::string(kAssetSha),
        "sha256sum's binary marker");
  std::string upper(kSums);
  for (std::size_t i = 0; i < 64; ++i) upper[i] = static_cast<char>(std::toupper(upper[i]));
  Check(SumsDigestFor(upper, kAsset) == std::string(kAssetSha), "uppercase hex reads lowercase");
}

void Decisions() {
  using V = SignatureVerdict;
  auto check = [](const std::optional<std::string>& sums, const std::optional<std::string>& sig,
                  std::string_view actual, bool required, const PublicKey& key) {
    return CheckReleaseSignature(sums, sig, kAsset, actual, required, key).verdict;
  };
  Check(check(kSums, kSig, kAssetSha, false, kTestKey) == V::Verified, "signed and matching verifies");
  Check(check(kSums, kSig, kAssetSha, true, kTestKey) == V::Verified, "and so when required");
  Check(check(kSums, kSig.substr(0, kSig.size() - 1), kAssetSha, false, kTestKey) == V::Verified,
        "a .sig without its newline still reads");
  std::string upperActual(kAssetSha);
  for (char& c : upperActual) c = static_cast<char>(std::toupper(c));
  Check(check(kSums, kSig, upperActual, false, kTestKey) == V::Verified, "case of the hash is ignored");

  Check(check(kSums, kSig, kAssetSha, false, kOtherKey) == V::Refused, "another key refuses");
  Check(check(kSums, kSig, kAssetSha, false, ReleasePublicKey()) == V::Refused,
        "the test signature does not verify under the release key");
  std::string tampered = kSums;
  tampered[0] = '2';
  Check(check(tampered, kSig, kAssetSha, false, kTestKey) == V::Refused, "a changed SHA256SUMS refuses");
  Check(check(kSums + "\n", kSig, kAssetSha, false, kTestKey) == V::Refused,
        "a newline added to SHA256SUMS refuses: the signature is over its exact bytes");
  Check(check(kSums, kSig, std::string(64, '0'), false, kTestKey) == V::Refused,
        "an installer that is not its line refuses");
  Check(check(kSums, "not base64\n", kAssetSha, false, kTestKey) == V::Refused, "a malformed .sig refuses");
  Check(check(kSums, "TWFu\n", kAssetSha, false, kTestKey) == V::Refused, "a .sig of the wrong length refuses");
  Check(check(kSums, std::string(""), kAssetSha, false, kTestKey) == V::Refused, "an empty .sig refuses");
  Check(check(std::nullopt, kSig, kAssetSha, false, kTestKey) == V::Refused, ".sig without SHA256SUMS refuses");
  Check(CheckReleaseSignature(kSums, kSig, "URnetwork-2026.11.25-x86.msi", kAssetSha, false, kTestKey).verdict ==
            V::Refused,
        "a signed SHA256SUMS without the installer's line refuses");

  Check(check(std::nullopt, std::nullopt, kAssetSha, false, kTestKey) == V::Unsigned,
        "unsigned, not required: warn and go on");
  Check(check(kSums, std::nullopt, kAssetSha, false, kTestKey) == V::Unsigned,
        "SHA256SUMS alone, not required: warn and go on");
  Check(check(std::nullopt, std::nullopt, kAssetSha, true, kTestKey) == V::Refused, "unsigned, required: refused");
  Check(check(kSums, std::nullopt, kAssetSha, true, kTestKey) == V::Refused, "SHA256SUMS alone, required: refused");
  Check(!kRequireUpdateSignature, "the signature is staged: not yet required");
}

}  // namespace

int main() {
  Sha512KnownAnswers();
  Rfc8032();
  Base64();
  Sums();
  Decisions();
  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-signature-tests: " << gCases << " checks, "
            << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
