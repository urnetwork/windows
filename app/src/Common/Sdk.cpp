// SPDX-License-Identifier: MPL-2.0
#include "Sdk.h"

#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {

void SdkInit(bool isService, int64_t memoryLimitBytes) {
  const std::string logDir = Narrow(LogDir(isService).wstring());
  urnet::setLogDir(logDir);
  urnet::setMemoryLimit(memoryLimitBytes);
  LogInfo("sdk initialized: version={} logDir={} memLimit={}MB",
          urnet::version(), logDir, memoryLimitBytes / (1024 * 1024));
}

}  // namespace urnw
