// The bytes of the payout key's store on Windows (BittensorKeyStore.cpp): the
// file one network's item lives in, what is in it, and the notes beside it.
//
// The store is the sdk's BittensorLocalKeyStore: one item per network id, which
// must outlive a sign-out, be readable by this user only, and never roam. Each
// item is a file under %LOCALAPPDATA%\URnetwork\app\bittensor_key, beside the
// sdk's storage rather than in it, since a sign-out clears that. The phrase in
// it is protected with DPAPI for the current user (CryptProtectData, with this
// network's entropy), so the file means nothing to another user, another
// machine, or another network's item; the address beside it is no secret, so
// the card can name the key without unprotecting anything. The notes the sdk
// keeps (addresses and numbers, never a phrase) are a second, plain file.
//
// Pure and header-only so tools/bittensor-key-store-format-tests.cpp runs it on
// any host: the Win32 half (files and DPAPI) cannot be built off Windows.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace urnw::btkeystore {

// The first line of an item: what the file is, and its format's version.
inline constexpr std::string_view kItemMagic = "URNETBTKEY1\n";

// The DPAPI description the protected phrase carries.
inline constexpr wchar_t kProtectDescription[] = L"URnetwork Bittensor key";

// The longest network id the store takes; the server's ids are UUIDs.
inline constexpr size_t kMaxNetworkIdLength = 64;

// The longest file the store reads: an item is the magic, an ss58 address and a
// DPAPI blob of a 12-word phrase, a few hundred bytes.
inline constexpr size_t kMaxFileBytes = 64 * 1024;

// The name of a network's files, the network id itself, or nothing for an id
// that could name something else: only letters, digits and '-', 1 to 64 of
// them. The store refuses anything else, so no id reaches outside its folder.
inline std::optional<std::string> FileStem(std::string_view networkId) {
  if (networkId.empty() || kMaxNetworkIdLength < networkId.size()) return std::nullopt;
  for (const char c : networkId) {
    const bool letter = ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
    const bool digit = '0' <= c && c <= '9';
    if (!letter && !digit && c != '-') return std::nullopt;
  }
  return std::string(networkId);
}

// The entropy the phrase is protected with: a constant of this store and the
// network id, so an item copied over another network's file does not unprotect.
inline std::string Entropy(std::string_view networkId) {
  std::string entropy("URnetwork Windows Bittensor key v1");
  entropy.push_back('\0');
  entropy.append(networkId);
  return entropy;
}

// An item as the file holds it.
struct Item {
  std::string address;
  std::string protectedPhrase;  // the DPAPI blob

  bool operator==(const Item&) const = default;
};

// An address the item can carry: ss58 is base58, which has no line break or
// space, and nothing else is ever stored as one.
inline bool AddressFits(std::string_view address) {
  if (address.empty() || 128 < address.size()) return false;
  for (const char c : address) {
    const bool letter = ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
    const bool digit = '0' <= c && c <= '9';
    if (!letter && !digit) return false;
  }
  return true;
}

// The file's bytes: the magic, the address on its own line, then the blob.
inline std::optional<std::string> EncodeItem(const Item& item) {
  if (!AddressFits(item.address) || item.protectedPhrase.empty()) return std::nullopt;
  std::string bytes(kItemMagic);
  bytes.append(item.address);
  bytes.push_back('\n');
  bytes.append(item.protectedPhrase);
  return bytes;
}

// The item in a file's bytes, or nothing for bytes that are not one: the store
// then says it cannot answer, which is never "no key".
inline std::optional<Item> DecodeItem(std::string_view bytes) {
  if (kMaxFileBytes < bytes.size() || !bytes.starts_with(kItemMagic)) return std::nullopt;
  bytes.remove_prefix(kItemMagic.size());
  const size_t end = bytes.find('\n');
  if (end == std::string_view::npos) return std::nullopt;
  Item item;
  item.address = std::string(bytes.substr(0, end));
  item.protectedPhrase = std::string(bytes.substr(end + 1));
  if (!AddressFits(item.address) || item.protectedPhrase.empty()) return std::nullopt;
  return item;
}

// ---- the notes ---------------------------------------------------------------------

using Notes = std::map<std::string, std::string>;

// A note's name or value the file can hold: one line, no tab. The sdk's notes
// are names like saved_address and values that are addresses or numbers.
inline bool NoteFits(std::string_view text) {
  if (512 < text.size()) return false;
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == '\t' || c == '\0') return false;
  }
  return true;
}

// "name\tvalue\n" for each note, in name order.
inline std::string EncodeNotes(const Notes& notes) {
  std::string bytes;
  for (const auto& [name, value] : notes) {
    bytes.append(name);
    bytes.push_back('\t');
    bytes.append(value);
    bytes.push_back('\n');
  }
  return bytes;
}

// The notes in a file's bytes. A line that is not one is dropped, as is an
// empty value: a note written "" is no note.
inline Notes DecodeNotes(std::string_view bytes) {
  Notes notes;
  if (kMaxFileBytes < bytes.size()) return notes;
  while (!bytes.empty()) {
    size_t end = bytes.find('\n');
    const std::string_view line = bytes.substr(0, end);
    bytes.remove_prefix(end == std::string_view::npos ? bytes.size() : end + 1);
    const size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0) continue;
    const std::string_view name = line.substr(0, tab);
    const std::string_view value = line.substr(tab + 1);
    if (value.empty() || !NoteFits(name) || !NoteFits(value)) continue;
    notes[std::string(name)] = std::string(value);
  }
  return notes;
}

// The notes with `name` written: "" removes it. False, changing nothing, for a
// name or value the file cannot hold.
inline bool WriteNote(Notes& notes, std::string_view name, std::string_view value) {
  if (name.empty() || !NoteFits(name) || !NoteFits(value)) return false;
  if (value.empty()) {
    notes.erase(std::string(name));
  } else {
    notes[std::string(name)] = std::string(value);
  }
  return true;
}

}  // namespace urnw::btkeystore
