// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorKeyStore.h"

#include <wincrypt.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

#include "BittensorKeyStoreFormat.h"
#include "Log.h"
#include "Paths.h"

namespace urnw {
namespace {

// %LOCALAPPDATA%\URnetwork\app\bittensor_key: this user's, on this machine, and
// beside the sdk's storage, which a sign-out clears, rather than in it.
std::filesystem::path StoreDir() {
  return StorageRoot(/*isService=*/false) / L"bittensor_key";
}

// A network's file, or nothing for an id the store refuses (FileStem).
std::optional<std::filesystem::path> FilePath(std::string const& networkId, std::wstring_view suffix) {
  const std::optional<std::string> stem = btkeystore::FileStem(networkId);
  if (!stem) return std::nullopt;
  // FileStem passes ASCII letters, digits and '-' only
  std::wstring name(stem->begin(), stem->end());
  name.append(suffix);
  return StoreDir() / name;
}

void Wipe(std::string& secret) {
  if (!secret.empty()) ::SecureZeroMemory(secret.data(), secret.size());
  secret.clear();
}

enum class Read { Ok, Missing, Failed };

Read ReadBytes(std::filesystem::path const& path, std::string& bytes) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Read::Missing : Read::Failed;
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > btkeystore::kMaxFileBytes) {
    ::CloseHandle(file);
    return Read::Failed;
  }
  bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                               &read, nullptr) &&
                                    read == bytes.size());
  ::CloseHandle(file);
  return ok ? Read::Ok : Read::Failed;
}

// Writes `bytes` whole beside `path` and moves them into place, so a crash
// leaves the old file or the new one, never half of one. `replace` false never
// replaces a file that is there: the move fails instead.
bool WriteBytes(std::filesystem::path const& path, std::string_view bytes, bool replace) {
  if (bytes.size() > std::numeric_limits<DWORD>::max()) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temp = path;
  temp += L".tmp." + std::to_wstring(::GetCurrentProcessId());
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool wrote = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) &&
                     written == bytes.size();
  const bool flushed = wrote && ::FlushFileBuffers(file);
  ::CloseHandle(file);
  const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
  if (!flushed || !::MoveFileExW(temp.c_str(), path.c_str(), flags)) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

bool RemoveFile(std::filesystem::path const& path) {
  if (::DeleteFileW(path.c_str())) return true;
  const DWORD error = ::GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

DATA_BLOB BlobOf(std::string_view bytes) {
  DATA_BLOB blob{};
  blob.cbData = static_cast<DWORD>(bytes.size());
  blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()));
  return blob;
}

// DPAPI for the current user: never CRYPTPROTECT_LOCAL_MACHINE, which any user
// of the machine could unprotect. No prompt ever shows.
std::optional<std::string> Protect(std::string_view phrase, std::string const& entropyBytes) {
  if (phrase.empty() || phrase.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input = BlobOf(phrase);
  DATA_BLOB entropy = BlobOf(entropyBytes);
  DATA_BLOB output{};
  if (!::CryptProtectData(&input, btkeystore::kProtectDescription, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  std::string protectedPhrase(reinterpret_cast<const char*>(output.pbData), output.cbData);
  ::LocalFree(output.pbData);
  return protectedPhrase;
}

std::optional<std::string> Unprotect(std::string_view protectedPhrase, std::string const& entropyBytes) {
  if (protectedPhrase.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input = BlobOf(protectedPhrase);
  DATA_BLOB entropy = BlobOf(entropyBytes);
  DATA_BLOB output{};
  if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
    return std::nullopt;
  }
  std::string phrase(reinterpret_cast<const char*>(output.pbData), output.cbData);
  ::SecureZeroMemory(output.pbData, output.cbData);
  ::LocalFree(output.pbData);
  return phrase;
}

std::string Unavailable() { return std::string(urnet::BittensorLocalKeyStoreUnavailable); }

// ---- the sdk's calls -------------------------------------------------------------
//
// Plain functions with no state of their own: the store they reach lives as
// long as the process, so a closed flow's goroutine never calls into freed
// memory. An exception never crosses into the sdk.

std::string Text(const char* text) { return text ? std::string(text) : std::string(); }

bool AddKeyThunk(void*, const char* networkId, const char* mnemonic, const char* address) noexcept {
  std::string phrase;
  bool added = false;
  try {
    phrase = Text(mnemonic);
    added = BittensorKeyStore::Shared().AddKey(Text(networkId), phrase, Text(address));
  } catch (...) {
    added = false;
  }
  Wipe(phrase);
  return added;
}

bool DeleteKeyThunk(void*, const char* networkId) noexcept {
  try {
    return BittensorKeyStore::Shared().DeleteKey(Text(networkId));
  } catch (...) {
    return false;
  }
}

char* KeyAddressThunk(void*, const char* networkId) noexcept {
  try {
    return urnet_copy_string(BittensorKeyStore::Shared().KeyAddress(Text(networkId)).c_str());
  } catch (...) {
    return urnet_copy_string(urnet::BittensorLocalKeyStoreUnavailable);
  }
}

char* KeyPhraseThunk(void*, const char* networkId) noexcept {
  std::string phrase;
  char* copy = nullptr;
  try {
    phrase = BittensorKeyStore::Shared().KeyPhrase(Text(networkId));
    copy = urnet_copy_string(phrase.c_str());
  } catch (...) {
    copy = urnet_copy_string(urnet::BittensorLocalKeyStoreUnavailable);
  }
  Wipe(phrase);
  return copy;
}

char* ReadNoteThunk(void*, const char* networkId, const char* name) noexcept {
  try {
    return urnet_copy_string(BittensorKeyStore::Shared().ReadNote(Text(networkId), Text(name)).c_str());
  } catch (...) {
    return urnet_copy_string("");
  }
}

void WriteNoteThunk(void*, const char* networkId, const char* name, const char* value) noexcept {
  try {
    BittensorKeyStore::Shared().WriteNote(Text(networkId), Text(name), Text(value));
  } catch (...) {
  }
}

// The flow's listener: one for the process (SetBittensorKeyFlowListener).
std::mutex& ListenerMutex() {
  static std::mutex mutex;
  return mutex;
}

std::function<void()>& Listener() {
  static std::function<void()> listener;
  return listener;
}

void FlowChangedThunk(void*) noexcept {
  try {
    std::function<void()> listener;
    {
      std::lock_guard lock(ListenerMutex());
      listener = Listener();
    }
    if (listener) listener();
  } catch (...) {
  }
}

}  // namespace

BittensorKeyStore& BittensorKeyStore::Shared() {
  // never destroyed: the sdk may call into it from a goroutine at any time
  static BittensorKeyStore* store = new BittensorKeyStore();
  return *store;
}

bool BittensorKeyStore::AddKey(std::string const& networkId, std::string const& mnemonic,
                               std::string const& address) {
  const auto path = FilePath(networkId, L".key");
  if (!path || mnemonic.empty() || !btkeystore::AddressFits(address)) return false;
  std::lock_guard lock(mutex_);
  if (::GetFileAttributesW(path->c_str()) != INVALID_FILE_ATTRIBUTES) return false;  // never replaces
  const std::optional<std::string> protectedPhrase = Protect(mnemonic, btkeystore::Entropy(networkId));
  if (!protectedPhrase) {
    LogWarn("bittensor key: the store could not protect a new item");
    return false;
  }
  const std::optional<std::string> bytes = btkeystore::EncodeItem({address, *protectedPhrase});
  if (!bytes) return false;
  if (!WriteBytes(*path, *bytes, /*replace=*/false)) {
    LogWarn("bittensor key: the store could not write a new item");
    return false;
  }
  return true;
}

std::string BittensorKeyStore::KeyPhrase(std::string const& networkId) {
  const auto path = FilePath(networkId, L".key");
  if (!path) return Unavailable();
  std::lock_guard lock(mutex_);
  std::string bytes;
  switch (ReadBytes(*path, bytes)) {
    case Read::Missing:
      return std::string();
    case Read::Failed:
      return Unavailable();
    case Read::Ok:
      break;
  }
  const std::optional<btkeystore::Item> item = btkeystore::DecodeItem(bytes);
  if (!item) return Unavailable();
  std::optional<std::string> phrase = Unprotect(item->protectedPhrase, btkeystore::Entropy(networkId));
  if (!phrase || phrase->empty()) return Unavailable();
  return std::move(*phrase);
}

std::string BittensorKeyStore::KeyAddress(std::string const& networkId) {
  const auto path = FilePath(networkId, L".key");
  if (!path) return Unavailable();
  std::lock_guard lock(mutex_);
  std::string bytes;
  switch (ReadBytes(*path, bytes)) {
    case Read::Missing:
      return std::string();
    case Read::Failed:
      return Unavailable();
    case Read::Ok:
      break;
  }
  const std::optional<btkeystore::Item> item = btkeystore::DecodeItem(bytes);
  if (!item) return Unavailable();
  return item->address;
}

bool BittensorKeyStore::DeleteKey(std::string const& networkId) {
  const auto path = FilePath(networkId, L".key");
  if (!path) return false;
  std::lock_guard lock(mutex_);
  return RemoveFile(*path);
}

std::string BittensorKeyStore::ReadNote(std::string const& networkId, std::string const& name) {
  const auto path = FilePath(networkId, L".notes");
  if (!path) return std::string();
  std::lock_guard lock(mutex_);
  std::string bytes;
  if (ReadBytes(*path, bytes) != Read::Ok) return std::string();
  const btkeystore::Notes notes = btkeystore::DecodeNotes(bytes);
  const auto found = notes.find(name);
  return found == notes.end() ? std::string() : found->second;
}

void BittensorKeyStore::WriteNote(std::string const& networkId, std::string const& name,
                                  std::string const& value) {
  const auto path = FilePath(networkId, L".notes");
  if (!path) return;
  std::lock_guard lock(mutex_);
  std::string bytes;
  btkeystore::Notes notes;
  switch (ReadBytes(*path, bytes)) {
    case Read::Failed:
      // writing over notes that could not be read would drop the others
      LogWarn("bittensor key: the store could not read its notes; a note was not written");
      return;
    case Read::Ok:
      notes = btkeystore::DecodeNotes(bytes);
      break;
    case Read::Missing:
      break;
  }
  if (!btkeystore::WriteNote(notes, name, value)) return;
  if (notes.empty()) {
    RemoveFile(*path);
    return;
  }
  if (!WriteBytes(*path, btkeystore::EncodeNotes(notes), /*replace=*/true)) {
    LogWarn("bittensor key: the store could not write its notes");
  }
}

urnet::BittensorLocalKeyFlow NewBittensorKeyFlow(urnet::Api const* api, std::string const& networkId) {
  return urnet::BittensorLocalKeyFlow(urnet_new_bittensor_local_key_flow(
      api ? api->handle() : 0, networkId.c_str(), &AddKeyThunk, &DeleteKeyThunk, &KeyAddressThunk,
      &KeyPhraseThunk, &ReadNoteThunk, &WriteNoteThunk, nullptr));
}

void SetBittensorKeyFlowListener(std::function<void()> listener) {
  std::lock_guard lock(ListenerMutex());
  Listener() = std::move(listener);
}

urnet::Sub ListenToBittensorKeyFlow(urnet::BittensorLocalKeyFlow const& flow) {
  if (!flow) return urnet::Sub();
  return urnet::Sub(urnet_bittensor_local_key_flow_add_bittensor_local_key_flow_listener(
      flow.handle(), &FlowChangedThunk, nullptr));
}

bool BittensorKeyNeedsBackup(std::string const& networkId) {
  if (networkId.empty()) return false;
  try {
    urnet::BittensorLocalKeyFlow flow = NewBittensorKeyFlow(nullptr, networkId);
    if (!flow) return false;
    const bool needsBackup = flow.needsBackup();
    flow.close();
    return needsBackup;
  } catch (std::exception const& e) {
    LogWarn("bittensor key: the backup check failed: {}", e.what());
  }
  return false;
}

bool ForgetBittensorKey(std::string const& networkId) {
  if (networkId.empty()) return false;
  try {
    urnet::BittensorLocalKeyFlow flow = NewBittensorKeyFlow(nullptr, networkId);
    if (!flow) return false;
    const bool gone = flow.forgetNetwork();
    flow.close();
    return gone;
  } catch (std::exception const& e) {
    LogWarn("bittensor key: forgetting the network's key failed: {}", e.what());
  }
  return false;
}

}  // namespace urnw
