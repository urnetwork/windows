// IS THE NATIVE-FAULT EVIDENCE CHANNEL ACTUALLY OPEN? ASK THE MACHINE, SAY SO.
//
// WHY THIS EXISTS. Clearing SEM_NOGPFAULTERRORBOX (Service/main.cpp) restores
// this process's ability to reach Windows Error Reporting. That is necessary
// and it is not sufficient: what WER then PRODUCES depends entirely on machine
// configuration this process does not own.
//
//   * If HKLM\...\Windows Error Reporting\LocalDumps is not configured, a
//     native fault yields an Application Error 1000 in the event log and a
//     report folder — and NO crash dump on disk. There is no stack, no
//     register state, nothing to open in a debugger.
//   * If ...\Windows Error Reporting!Disabled is 1, the unblinding buys
//     nothing at all and the death stays as silent as it was before.
//
// Measured on the owner's box while verifying task #39: the LocalDumps key
// EXISTS, but carries no values of its own — every value under it belongs to a
// per-executable subkey planted by unrelated software (Razer, NVIDIA,
// BlueStacks). There is no urnetworkd.exe subkey. That is the most misleading
// shape this configuration can take, because a probe that merely asks "does
// LocalDumps exist?" answers yes and reports a channel that may write nothing.
// DefaultConsent is 4 (send automatically), so the WER report itself is
// generated and uploaded, and the local copy archived without a dump.
//
// AND THAT IS THE DANGEROUS CASE, NOT THE HARMLESS ONE. An investigator told
// "WER is unblinded now" looks in %LOCALAPPDATA%\CrashDumps after the next
// death, finds an empty folder, and concludes there was no native fault —
// which is exactly the false negative that cost four investigations. A channel
// that is quietly shut is worse than one known to be shut, so this process
// states the channel's real condition in its own log, every start, before
// anything can go wrong.
//
// This module only READS the registry. Opening the channel needs an elevated
// write to HKLM, which is the operator's decision to make; the log line hands
// them the exact command and this code does not run it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw {

// What the machine says will happen to this process when it faults natively.
// Defaults describe the WORST honest reading, so a probe that cannot read the
// registry at all does not report a channel that is open.
struct CrashDumpChannel {
  // HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting!Disabled == 1.
  // Kills reporting outright — the SEM_NOGPFAULTERRORBOX clear is then moot.
  bool werDisabled = false;
  // ...\Windows Error Reporting\LocalDumps exists.
  bool localDumpsGlobal = false;
  // …and that key carries at least one of DumpFolder / DumpType / DumpCount
  // ITSELF. This is the distinction that matters and the one a naive probe
  // misses: the key is also created as a mere container by any installer that
  // wants a per-executable subkey for its own program, and in that shape it
  // says nothing whatsoever about what happens to us.
  bool localDumpsGlobalHasValues = false;
  // ...\Windows Error Reporting\LocalDumps\<exe> exists. Takes precedence over
  // the global key for this executable, which is why it is asked separately.
  bool localDumpsForThisExe = false;
  // 0 = custom, 1 = mini (the Windows default when the key exists but DumpType
  // does not), 2 = full. Only meaningful when one of the flags above is set.
  unsigned long dumpType = 1;
  // DumpFolder as configured, unexpanded. Empty means the Windows default,
  // %LOCALAPPDATA%\CrashDumps — which for a LocalSystem service resolves under
  // C:\Windows\System32\config\systemprofile, not under the operator's profile.
  std::wstring dumpFolder;
};

// Read the three keys above. Never throws, never writes; an unreadable key
// reads as absent. `exeName` is the leaf file name, e.g. L"urnetworkd.exe".
CrashDumpChannel ProbeCrashDumpChannel(const wchar_t* exeName);

// The sentence that goes in the log. PURE — no registry, no clock — so
// `urnetworkd selftest` can prove the verdicts on a machine whose real
// configuration is whatever it happens to be.
//
// It says three things in this order, because that is the order the reader
// needs them in after a death: what WILL exist, what will NOT, and the one
// command that changes the answer.
std::string DescribeCrashDumpChannel(const CrashDumpChannel& channel,
                                     std::string_view exeName);

}  // namespace urnw
