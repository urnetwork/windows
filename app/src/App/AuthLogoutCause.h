// The cause of an AuthLogout, read where the sdk reports it: inside the
// listener, on the sdk's thread, before anything is marshalled
// (AuthLogoutNotice.h). The sdk sets it (urnet::AuthLogoutCauseSessionRevoked,
// or "") before its logout listeners run and keeps it through the sign-out
// that follows, but a new sign-in clears it, so it is not left for the UI
// thread to read later.
//
// Through the c abi by the handle the listener was added on, as
// UploadLogsThroughDeviceRemote (SdkHost.cpp) reaches its device: a listener
// reads nothing of SdkHost, whose api_ and device_ are replaced under its
// lock. These are the calls Api::getAuthLogoutCause and
// Device::getAuthLogoutCause make. A listener still in flight while its
// session is torn down may hold a handle released since, which reads as "".
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <urnetwork_sdk.h>

namespace urnw::authlogout {

// The sdk's string is the caller's to free.
inline std::string TakeCause(char* cause) {
  if (cause == nullptr) return {};
  std::string value(cause);
  urnet_free_string(cause);
  return value;
}

inline std::string ApiCause(uint64_t api) {
  return TakeCause(urnet_api_get_auth_logout_cause(api));
}

inline std::string DeviceCause(uint64_t device) {
  return TakeCause(urnet_device_get_auth_logout_cause(device));
}

}  // namespace urnw::authlogout
