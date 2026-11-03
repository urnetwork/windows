// The payout key's store on Windows: the sdk's BittensorLocalKeyStore (sdk
// bittensor_local_key_flow.go, apple/BTEASY.md Storage and Porting), and the
// sdk's flow made over it.
//
// One item per network id, in %LOCALAPPDATA% (this user, this machine, never a
// roaming profile), beside the sdk's storage rather than in it, so a sign-out
// leaves it and the key comes back when that network signs in again. The phrase
// is protected with DPAPI for the current user; the address beside it is no
// secret. The file layout is BittensorKeyStoreFormat.h's.
//
// This file holds no decisions: the sdk writes and reads back before it signs,
// and checks a deletion by reading again. Nothing here logs a phrase, an
// address or a note: a failure says only which call failed.
//
// The store is one object for the life of the process, and the sdk reaches it
// through plain functions with no per-flow state, so a goroutine of a flow that
// is already closed can never call into freed memory. The same goes for the
// flow's listener: one process-wide hook, which the Earnings page points at
// itself (SetBittensorKeyFlowListener).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "Sdk.h"

namespace urnw {

class BittensorKeyStore {
 public:
  // The one store, for the life of the process.
  static BittensorKeyStore& Shared();

  // The sdk's BittensorLocalKeyStore. Any thread.
  // Adds the network's item; never replaces one. False if one is there or the
  // store refused.
  bool AddKey(std::string const& networkId, std::string const& mnemonic, std::string const& address);
  // The item's phrase: "" with no item, urnet::BittensorLocalKeyStoreUnavailable
  // when the store can't answer.
  std::string KeyPhrase(std::string const& networkId);
  // The item's address, without unprotecting the phrase; "" and unavailable as above.
  std::string KeyAddress(std::string const& networkId);
  // Removes the item. No item is not a refusal.
  bool DeleteKey(std::string const& networkId);
  // The non-secret notes the sdk keeps by network id; "" for none, and a value
  // of "" removes one.
  std::string ReadNote(std::string const& networkId, std::string const& name);
  void WriteNote(std::string const& networkId, std::string const& name, std::string const& value);

 private:
  BittensorKeyStore() = default;
  std::mutex mutex_;
};

// The sdk's flow for `networkId` over the shared store (sdk
// NewBittensorLocalKeyFlow). `api` null makes a flow that only asks, as the
// sign-out and account deletion warnings do: it signs nothing.
urnet::BittensorLocalKeyFlow NewBittensorKeyFlow(urnet::Api const* api, std::string const& networkId);

// The flow's changes reach `listener`, on whichever thread the sdk publishes
// on, until the Sub is closed. One listener for the process: the Earnings page
// sets it with its own marshal to the UI thread (SetBittensorKeyFlowListener),
// and a change from a flow it has already closed only makes it read its
// current one again.
void SetBittensorKeyFlowListener(std::function<void()> listener);
urnet::Sub ListenToBittensorKeyFlow(urnet::BittensorLocalKeyFlow const& flow);

// The network has a key on this device whose phrase is not known to be saved
// (sdk NeedsBackup): Sign out and Delete account warn on it. False for none, an
// empty id, or a store that can't answer.
bool BittensorKeyNeedsBackup(std::string const& networkId);

// After an account deletion: the key and the sdk's notes go (sdk ForgetNetwork).
// False if the key could not be shown gone.
bool ForgetBittensorKey(std::string const& networkId);

}  // namespace urnw
