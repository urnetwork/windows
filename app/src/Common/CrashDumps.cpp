// SPDX-License-Identifier: MPL-2.0
#include "CrashDumps.h"

#include <format>
#include <string>

// WIN32_LEAN_AND_MEAN is already on this project's command line; redefining it
// here only produces a C4005 (same note as Sdk.cpp).
#include <windows.h>

#include "Strings.h"

namespace urnw {
namespace {

constexpr const wchar_t* kWerKey =
    L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting";
constexpr const wchar_t* kLocalDumpsKey =
    L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps";

// KEY_WOW64_64KEY explicitly. urnetworkd is x64 so it would get the 64-bit view
// anyway, but WER's configuration lives in the 64-bit hive only and a probe
// that reports "not configured" because it read the wrong hive would be a lie
// of exactly the kind this file exists to prevent.
bool OpenReadOnly(const wchar_t* subKey, HKEY* out) {
  return ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0,
                         KEY_READ | KEY_WOW64_64KEY, out) == ERROR_SUCCESS;
}

bool ReadDword(HKEY key, const wchar_t* name, unsigned long* out) {
  DWORD type = 0;
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (::RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS)
    return false;
  if (type != REG_DWORD) return false;
  *out = value;
  return true;
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
      ERROR_SUCCESS)
    return {};
  if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
  std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
  if (::RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(value.data()),
                         &bytes) != ERROR_SUCCESS)
    return {};
  value.resize(::wcsnlen(value.c_str(), value.size()));
  return value;
}

const char* DumpTypeName(unsigned long type) {
  switch (type) {
    case 0: return "custom";
    case 1: return "MINI — enough for a faulting stack, not for the heap";
    case 2: return "FULL";
    default: return "unrecognised";
  }
}

}  // namespace

CrashDumpChannel ProbeCrashDumpChannel(const wchar_t* exeName) {
  CrashDumpChannel out;

  HKEY wer = nullptr;
  if (OpenReadOnly(kWerKey, &wer)) {
    unsigned long disabled = 0;
    if (ReadDword(wer, L"Disabled", &disabled)) out.werDisabled = disabled != 0;
    ::RegCloseKey(wer);
  }

  // The per-executable key wins over the global one, so it is read second and
  // its values overwrite. Absent both, everything below stays at the "no dump"
  // reading the struct defaults to.
  HKEY dumps = nullptr;
  if (OpenReadOnly(kLocalDumpsKey, &dumps)) {
    out.localDumpsGlobal = true;
    unsigned long type = 0;
    unsigned long count = 0;
    const bool hasType = ReadDword(dumps, L"DumpType", &type);
    if (hasType) out.dumpType = type;
    out.dumpFolder = ReadString(dumps, L"DumpFolder");
    // "Does the key exist" is NOT the question. Any installer that wants a
    // per-executable subkey for its own program creates this key as a side
    // effect, and in that shape it is an empty container that configures
    // nothing. Only its own values mean someone turned dumps on machine-wide.
    out.localDumpsGlobalHasValues =
        hasType || !out.dumpFolder.empty() || ReadDword(dumps, L"DumpCount", &count);
    ::RegCloseKey(dumps);
  }

  if (exeName && *exeName) {
    const std::wstring perExe = std::wstring(kLocalDumpsKey) + L"\\" + exeName;
    HKEY mine = nullptr;
    if (OpenReadOnly(perExe.c_str(), &mine)) {
      out.localDumpsForThisExe = true;
      unsigned long type = 0;
      if (ReadDword(mine, L"DumpType", &type)) out.dumpType = type;
      const std::wstring folder = ReadString(mine, L"DumpFolder");
      if (!folder.empty()) out.dumpFolder = folder;
      ::RegCloseKey(mine);
    }
  }
  return out;
}

std::string DescribeCrashDumpChannel(const CrashDumpChannel& channel,
                                     std::string_view exeName) {
  // WER off entirely is reported first and alone. Nothing below it is true when
  // this is set, and a reader who takes the "Event 1000 will exist" half of a
  // longer sentence away with them is worse off than one who read nothing.
  if (channel.werDisabled) {
    return std::format(
        "service: NATIVE-FAULT EVIDENCE IS OFF AT THE MACHINE LEVEL. "
        "HKLM\\{}!Disabled is 1, so Windows Error Reporting will not run for "
        "ANY process here: a native fault in {} produces no report, no "
        "Application Error 1000 and no dump, and clearing SEM_NOGPFAULTERRORBOX "
        "buys nothing while that is true. To reopen the channel, from an "
        "ELEVATED prompt: reg add \"HKLM\\{}\" /v Disabled /t REG_DWORD /d 0 /f",
        "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", exeName,
        "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting");
  }

  // The one command that removes every ambiguity below. Per-executable, because
  // a per-executable subkey is the only form of this setting whose effect on
  // THIS process is documented and unconditional.
  const std::string fix = std::format(
      "From an ELEVATED prompt, once: reg add "
      "\"HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error "
      "Reporting\\LocalDumps\\{}\" /v DumpFolder /t REG_EXPAND_SZ /d "
      "C:\\ProgramData\\URnetwork\\service\\crashdumps /f  &&  reg add "
      "\"HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error "
      "Reporting\\LocalDumps\\{}\" /v DumpType /t REG_DWORD /d 2 /f  &&  reg add "
      "\"HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error "
      "Reporting\\LocalDumps\\{}\" /v DumpCount /t REG_DWORD /d 5 /f   "
      "(DumpType 2 is a FULL dump: this process is half Go, and a mini dump of a "
      "cgo thread routinely lacks the frames that name the fault. Takes effect "
      "on the next fault — no restart, no reinstall.)",
      exeName, exeName, exeName);

  if (!channel.localDumpsForThisExe && !channel.localDumpsGlobalHasValues) {
    // Two machine states share this verdict, and they must, because they have
    // the same consequence and only one of them LOOKS reassuring: the key is
    // absent, or the key exists purely as a container for other applications'
    // per-executable subkeys and carries no values of its own. Reporting the
    // second as "configured" would be the exact failure this file exists to
    // stop — a channel believed open, checked after a death, found empty, and
    // read as proof there was no fault.
    return std::format(
        "service: crash-dump channel — WER will report a native fault but is "
        "NOT KNOWN TO WRITE A DUMP FILE for this process. {} So after a fault, "
        "expect an Application Error 1000 in the event log and a WER report "
        "folder, and do NOT expect anything with a stack in it. AN EMPTY "
        "CrashDumps FOLDER IS NOT EVIDENCE THAT NOTHING CRASHED — that false "
        "negative is what four investigations of task #39 have been paying for. "
        "{}",
        channel.localDumpsGlobal
            ? "HKLM\\...\\Windows Error Reporting\\LocalDumps exists, but "
              "carries no values of its own — it is there as a container for "
              "other programs' per-executable subkeys, and there is no subkey "
              "for this executable, so nothing here is configured for us."
            : "HKLM\\...\\Windows Error Reporting\\LocalDumps does not exist at "
              "all.",
        fix);
  }

  // Configured. Say exactly where the file will land, because the default is
  // not where anyone looks: a LocalSystem service's %LOCALAPPDATA% is under
  // C:\Windows\System32\config\systemprofile, not under the operator's profile.
  const std::string folder =
      channel.dumpFolder.empty()
          ? std::string(
                "C:\\Windows\\System32\\config\\systemprofile\\AppData\\Local\\"
                "CrashDumps (the Windows default — a LocalSystem service's "
                "%LOCALAPPDATA%, NOT the operator's)")
          : Narrow(channel.dumpFolder);

  return std::format(
      "service: crash-dump channel OPEN — a native fault should write a dump to "
      "{}. Scope: {}. DumpType={} ({}). Together with the SEM_NOGPFAULTERRORBOX "
      "clear above, that is a stack for the next death instead of a silence.{}",
      folder,
      channel.localDumpsForThisExe
          ? std::format("configured for THIS executable ({}), which is the "
                        "unconditional form of the setting",
                        exeName)
          : std::string("the machine-wide LocalDumps values, which apply only "
                        "because no per-executable subkey overrides them"),
      channel.dumpType, DumpTypeName(channel.dumpType),
      channel.dumpType == 1
          ? std::format(" NOTE: a MINI dump of a half-Go process routinely lacks"
                        " the frames that name the fault. For this "
                        "investigation, make it a full dump: {}",
                        fix)
          : std::string());
}

}  // namespace urnw
