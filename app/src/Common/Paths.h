// Per-process storage and log locations. The SDK owns persistence via
// NetworkSpaceManager(storagePath); each process gets its own dir, as on macOS
// (the app and the network extension do not share a container).
//
//   App (per user):   %LOCALAPPDATA%\URnetwork
//   Service (SYSTEM): %ProgramData%\URnetwork
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>

namespace urnw {

// Root storage dir for the current process, created if missing.
//   isService = true  -> %ProgramData%\URnetwork\service
//   isService = false -> %LOCALAPPDATA%\URnetwork\app
std::filesystem::path StorageRoot(bool isService);

// SDK storage path (passed to NetworkSpaceManager). Subdir of StorageRoot.
std::filesystem::path SdkStorageDir(bool isService);

// glog log directory (passed to urnw::setLogDir). Subdir of StorageRoot.
std::filesystem::path LogDir(bool isService);

// Persisted last-good RPC session file (app side only).
std::filesystem::path RpcSessionFile();

// The APP's own preferences (app side only).
//
// Distinct from the SDK LocalState, and it has to be: LocalState is a fixed set
// of typed accessors compiled into the SDK (getRouteLocal, getProvideControlMode,
// getBlockActionOverrides, ...) with no generic key/value pair anywhere in the C
// ABI, so a preference that belongs to THIS CLIENT rather than to the SDK has
// nowhere to live in it. Advanced Mode is the first of those. Same
// one-small-json-file idiom as rpc_session.json and in the same per-worktree
// StorageRoot, so two agents' worktrees do not share one preferences file.
std::filesystem::path AppPrefsFile();

}  // namespace urnw
