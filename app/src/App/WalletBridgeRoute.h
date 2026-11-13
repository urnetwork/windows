// Which waiting flow a wallet-bridge return belongs to. The bridge is a pair of
// process-wide callbacks with no request id, so a return nobody waits for (the
// bridge page's "Return to URnetwork" after its automatic redirect, a tab from
// an abandoned or superseded attempt) is dropped, never turned into a sign-in.
// And a flow that was superseded while its challenge was on its way never opens
// the bridge afterwards (FlowSerial).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace urnw::bridge {

// every reason SdkHost::CancelPendingWalletFlows answers with starts with this
inline constexpr std::string_view kSupersededPrefix = "superseded by ";

constexpr bool IsSuperseded(std::string_view error) noexcept {
  return error.starts_with(kSupersededPrefix);
}

enum class PublicKeyRoute { Drop, SignForRequest, SignIn };

constexpr PublicKeyRoute RoutePublicKey(bool bittensor, bool signWaiting,
                                        bool walletSignInWaiting) noexcept {
  if (bittensor) return PublicKeyRoute::Drop;  // no connect hop to chain
  if (signWaiting) return PublicKeyRoute::SignForRequest;
  return walletSignInWaiting ? PublicKeyRoute::SignIn : PublicKeyRoute::Drop;
}

enum class SignatureRoute { Drop, AnswerRequest, SignIn };

constexpr SignatureRoute RouteSignature(bool signWaiting, bool walletSignInWaiting) noexcept {
  if (signWaiting) return SignatureRoute::AnswerRequest;
  return walletSignInWaiting ? SignatureRoute::SignIn : SignatureRoute::Drop;
}

// Every wallet-bridge flow takes the next number as it starts
// (SdkHost::CancelPendingWalletFlows). A continuation that resumes later - a
// challenge fetched over the network - may open the bridge, or answer anyone,
// only while its flow is still the current one. A superseded flow's late
// challenge must not open a tab over the current flow, overwrite the message it
// signs, reset its session or fail its request. Atomic: flows start on the UI
// thread and challenge continuations resume on SDK threads.
class FlowSerial {
 public:
  // a flow starts, and every earlier one is superseded
  uint64_t Start() noexcept { return current_.fetch_add(1) + 1; }
  // the number of the flow that started last
  uint64_t Current() const noexcept { return current_.load(); }
  bool IsCurrent(uint64_t flow) const noexcept { return current_.load() == flow; }

 private:
  std::atomic<uint64_t> current_{0};
};

}  // namespace urnw::bridge
