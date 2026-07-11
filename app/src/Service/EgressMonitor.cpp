// SPDX-License-Identifier: MPL-2.0
#include "EgressMonitor.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "Log.h"
#include "NetworkConfig.h"
#include "Sdk.h"

#pragma comment(lib, "iphlpapi.lib")

namespace urnw {

EgressMonitor::~EgressMonitor() { Stop(); }

bool EgressMonitor::Start() {
  Refresh();
  DWORD err = ::NotifyIpInterfaceChange(AF_UNSPEC, &EgressMonitor::OnChange,
                                        this, FALSE, &notifyHandle_);
  if (err != NO_ERROR) {
    LogError("egress: NotifyIpInterfaceChange failed: {}", err);
    return false;
  }
  return true;
}

void EgressMonitor::Stop() {
  if (notifyHandle_) {
    ::CancelMibChangeNotify2(notifyHandle_);
    notifyHandle_ = nullptr;
  }
}

void EgressMonitor::Refresh() {
  EgressInterfaces egress = NetworkConfig::DiscoverEgress(tunLuid_);
  urnet::setEgressInterfaceIndex(static_cast<int64_t>(egress.index4),
                                 static_cast<int64_t>(egress.index6));
  LogInfo("egress: bound to interfaces v4={} v6={}", egress.index4,
          egress.index6);
}

void __stdcall EgressMonitor::OnChange(void* context, MIB_IPINTERFACE_ROW*,
                                       MIB_NOTIFICATION_TYPE) {
  // Called on a system worker thread. Recompute the egress binding; the SDK
  // setter is atomic and cheap, so we can react to every change.
  auto* self = static_cast<EgressMonitor*>(context);
  if (self) self->Refresh();
}

}  // namespace urnw
