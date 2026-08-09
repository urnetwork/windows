// The build's own identity, stamped by CI and readable from code.
//
// CI passes /p:UrVersion=<full string> /p:UrVersionCode=<code> plus the three
// numeric date parts; app/Directory.Build.props turns them into UR_*
// preprocessor definitions for every TU and every .rc in the solution. This
// header is the one place those raw tokens become usable constants, with dev
// defaults when the definitions are absent entirely (an IDE parsing a single
// file, say) — so nothing else ever #ifdefs on how the build was invoked.
//
// UR_VERSION_RAW arrives as a BARE token (2026.8.9-101076420-beta), not a
// quoted literal: a \" escaped inside PreprocessorDefinitions does not survive
// MSBuild's command-line quoting on this toolchain — cl sees the value end at
// the backslash (verified for URN_WINDOWSAPPSDK_VERSION_RAW, App.vcxproj, the
// idiom this copies). Stringizing reproduces the exact spelling, because a
// number/minus/identifier token run with no interior whitespace stringizes to
// itself.
//
// The .rc files include this header too (RC_INVOKED guards the C++ half), so
// the VERSIONINFO resources and the code can never disagree about what version
// a binary is.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#define UR_VERSION_STRINGIZE_(x) #x
#define UR_VERSION_STRINGIZE(x) UR_VERSION_STRINGIZE_(x)

// The full version string in the release grammar, without the tag's leading v
// ("2026.8.9-101076420-beta"), or "0.0.0-dev" for any build CI did not stamp.
#ifndef UR_VERSION_STRING
#ifdef UR_VERSION_RAW
#define UR_VERSION_STRING UR_VERSION_STRINGIZE(UR_VERSION_RAW)
#else
#define UR_VERSION_STRING "0.0.0-dev"
#endif
#endif

// The android-parity numeric release code (seconds since the 2023-05-23
// founding, times ten). 0 means dev build.
#ifndef UR_VERSION_CODE
#define UR_VERSION_CODE 0
#endif

// The date parts, for the four 16-bit numeric FILEVERSION fields.
#ifndef UR_VER_MAJOR
#define UR_VER_MAJOR 0
#endif
#ifndef UR_VER_MINOR
#define UR_VER_MINOR 0
#endif
#ifndef UR_VER_PATCH
#define UR_VER_PATCH 0
#endif

#ifndef RC_INVOKED

#include <cstdint>

namespace urnw::version {

// The full version string ("0.0.0-dev" outside CI).
inline constexpr char kString[] = UR_VERSION_STRING;

// The numeric release code; kCode == 0 identifies a dev build, and dev builds
// never self-update. uint64_t, not int: the code is ~1e9 already and grows
// with the calendar, so a 32-bit signed carrier would roll negative within the
// product's lifetime.
inline constexpr std::uint64_t kCode = UR_VERSION_CODE;

}  // namespace urnw::version

#endif  // RC_INVOKED
