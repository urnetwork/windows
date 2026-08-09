// Build-time app configuration. Identity constants (service name, tray GUID,
// uri scheme) live in Common/Ids.h and must stay stable across releases; this is
// for values that vary by build or deployment.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::config {

// WalletConnect Cloud project id — one project id shared by every URnetwork
// client (see apple/NEXTSTEPS2.md). It is passed to the ur.io/wallet-connect
// bridge as `wc_project_id` so the bridge can pair with a wallet app (QR /
// mobile deep link).
//
// Empty is fine and is the common desktop case: the bridge then drives injected
// (browser-extension) wallets only — Bittensor Wallet, SubWallet, Talisman,
// polkadot-js for Bittensor; Phantom/Solflare for Solana. Nothing crashes and no
// button dies; only mobile wallet pairing is lost.
//
// Inject it on the build (CI / build machine, the way android takes it from
// local.properties) rather than committing it:
//   msbuild ... /p:UrnWalletConnectProjectId=<project id>
//
// App.vcxproj passes the id as a BARE token and it is stringized here: an
// MSBuild PreprocessorDefinition cannot carry `\"`-escaped quotes through to cl
// (verified — the value ends at the backslash and the TU fails to compile), so
// the quoting has to happen in the preprocessor instead.
#define URN_CONFIG_STR2(x) #x
#define URN_CONFIG_STR(x) URN_CONFIG_STR2(x)

#if defined(URN_WALLETCONNECT_PROJECT_ID_RAW)
inline constexpr const char* kWalletConnectProjectId =
    URN_CONFIG_STR(URN_WALLETCONNECT_PROJECT_ID_RAW);
#else
inline constexpr const char* kWalletConnectProjectId = "";
#endif

// Google OAuth client — a "Desktop app" client from the URnetwork Google Cloud
// project, used by the system-browser loopback flow in GoogleSignIn.cpp.
//
// Empty is the default and is NOT a broken state: GoogleSignIn::Configured()
// returns false, the network space reports sso_google=false, and the login
// screen HIDES the Google button rather than offering one that cannot work.
// A build that wants the button injects both on the command line, the same way
// the WalletConnect project id is injected:
//   msbuild ... /p:UrnGoogleOAuthClientId=<id>.apps.googleusercontent.com
//               /p:UrnGoogleOAuthClientSecret=<secret>
//
// The "secret" of a Google Desktop client is not confidential — it ships inside
// every copy of the binary and Google documents it as such. PKCE (RFC 7636) is
// what actually binds an authorization code to the process that asked for it,
// and GoogleSignIn always sends a code challenge.
#if defined(URN_GOOGLE_OAUTH_CLIENT_ID_RAW)
inline constexpr const char* kGoogleOAuthClientId =
    URN_CONFIG_STR(URN_GOOGLE_OAUTH_CLIENT_ID_RAW);
#else
inline constexpr const char* kGoogleOAuthClientId = "";
#endif

#if defined(URN_GOOGLE_OAUTH_CLIENT_SECRET_RAW)
inline constexpr const char* kGoogleOAuthClientSecret =
    URN_CONFIG_STR(URN_GOOGLE_OAUTH_CLIENT_SECRET_RAW);
#else
inline constexpr const char* kGoogleOAuthClientSecret = "";
#endif

// The GitHub repo the update checker polls for releases (beta-distribution
// spec §5): the beta fork today, and the whole upstream handoff is this one
// line — repoint it at urnetwork/<repo> when the fork graduates. Wide because
// it is spliced into WinHTTP request strings, which are UTF-16 end to end.
inline constexpr const wchar_t* kUpdateRepo = L"Ryanmello07/urnetwork-windows";

}  // namespace urnw::config
