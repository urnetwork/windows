// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "GoogleSignIn.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Config.h"
#include "Log.h"
#include "Strings.h"

namespace urnw {
namespace {

constexpr const char* kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const wchar_t* kTokenHost = L"oauth2.googleapis.com";
constexpr const wchar_t* kTokenPath = L"/token";
// openid+email is all the server needs from the id token; asking for less than
// the other clients would only make the consent screen inconsistent.
constexpr const char* kScope = "openid email profile";
// How long the user has to finish in the browser before the socket gives up.
constexpr DWORD kAcceptTimeoutMs = 5 * 60 * 1000;

std::string Esc(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string Unesc(const std::string& s) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

// base64url without padding — what PKCE (RFC 7636) and every OAuth server want.
std::string Base64Url(const uint8_t* data, size_t len) {
  DWORD n = 0;
  ::CryptBinaryToStringA(data, static_cast<DWORD>(len),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
  std::string s(n, '\0');
  if (!::CryptBinaryToStringA(data, static_cast<DWORD>(len),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n)) {
    return "";
  }
  s.resize(n);
  for (auto& c : s) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

std::vector<uint8_t> RandomBytes(size_t n) {
  std::vector<uint8_t> out(n);
  if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    // A predictable verifier defeats the point of PKCE, so this must not fall
    // back to rand(). Signal it by returning empty; the caller aborts.
    out.clear();
  }
  return out;
}

std::vector<uint8_t> Sha256(const std::string& input) {
  std::vector<uint8_t> digest(32);
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
    return {};
  const NTSTATUS st = ::BCryptHash(
      alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
      static_cast<ULONG>(input.size()), digest.data(), static_cast<ULONG>(digest.size()));
  ::BCryptCloseAlgorithmProvider(alg, 0);
  if (!BCRYPT_SUCCESS(st)) return {};
  return digest;
}

void OpenBrowser(const std::string& url) {
  const std::wstring w = Widen(url);
  ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The one-line page the browser is left on. Deliberately plain text with no
// script and no link: it is shown by the user's browser, not by this app, and
// nothing here is localized because the localization store is the app's.
constexpr const char* kDonePage =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\nContent-Length: 118\r\n\r\n"
    "<!doctype html><meta charset=utf-8><title>URnetwork</title>"
    "<body style=\"font-family:sans-serif\">You can close this tab.</body>";

// Pull `name` out of an HTTP request line's query string.
std::string QueryParam(const std::string& requestLine, const std::string& name) {
  const auto q = requestLine.find('?');
  if (q == std::string::npos) return "";
  const auto end = requestLine.find(' ', q);
  const std::string query =
      requestLine.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
  size_t i = 0;
  while (i < query.size()) {
    const auto amp = query.find('&', i);
    const std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const auto eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == name) return Unesc(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return "";
}

// POST the authorization code to Google's token endpoint. Returns the id_token,
// or sets `error`.
std::string ExchangeCode(const std::string& code, const std::string& verifier,
                         const std::string& redirectUri, std::string& error) {
  const std::string body =
      "grant_type=authorization_code&code=" + Esc(code) +
      "&client_id=" + Esc(config::kGoogleOAuthClientId) +
      (std::string(config::kGoogleOAuthClientSecret).empty()
           ? std::string()
           : "&client_secret=" + Esc(config::kGoogleOAuthClientSecret)) +
      "&code_verifier=" + Esc(verifier) + "&redirect_uri=" + Esc(redirectUri);

  HINTERNET session = ::WinHttpOpen(L"URnetwork", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = "could not open an https session";
    return "";
  }
  HINTERNET connection = ::WinHttpConnect(session, kTokenHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request =
      connection ? ::WinHttpOpenRequest(connection, L"POST", kTokenPath, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE)
                 : nullptr;
  std::string response;
  bool ok = false;
  if (request) {
    static constexpr const wchar_t* kFormHeader =
        L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (::WinHttpSendRequest(request, kFormHeader, static_cast<DWORD>(-1),
                             const_cast<char*>(body.data()),
                             static_cast<DWORD>(body.size()),
                             static_cast<DWORD>(body.size()), 0) &&
        ::WinHttpReceiveResponse(request, nullptr)) {
      DWORD available = 0;
      while (::WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), available, &read)) break;
        response.append(chunk, 0, read);
      }
      ok = true;
    }
  }
  if (request) ::WinHttpCloseHandle(request);
  if (connection) ::WinHttpCloseHandle(connection);
  ::WinHttpCloseHandle(session);

  if (!ok) {
    error = "the token request to Google failed";
    return "";
  }
  try {
    auto j = nlohmann::json::parse(response);
    if (j.contains("error")) {
      // error_description is Google's own words and is the only diagnostic the
      // user will get; it is not localizable and must not be swallowed.
      error = j.value("error_description", j.value("error", std::string("token error")));
      return "";
    }
    const std::string idToken = j.value("id_token", std::string());
    if (idToken.empty()) error = "Google returned no id token";
    return idToken;
  } catch (const std::exception&) {
    // Do NOT include the body: on a failure it can carry the code or a token.
    error = "could not read Google's token response";
    return "";
  }
}

}  // namespace

// One attempt. The listening socket is the cancellation handle: closesocket()
// from Cancel() makes the worker's accept() fail immediately, which is the only
// way to interrupt a blocking accept portably.
struct GoogleSignIn::Attempt {
  std::mutex mutex;
  SOCKET listener = INVALID_SOCKET;
  std::atomic<bool> cancelled{false};
  Done done;

  void Close() {
    std::scoped_lock lock(mutex);
    if (listener != INVALID_SOCKET) {
      ::closesocket(listener);
      listener = INVALID_SOCKET;
    }
  }
  // Deliver exactly once, whichever of the worker or Cancel gets there first.
  void Finish(std::string idToken, std::string error) {
    Done fn;
    {
      std::scoped_lock lock(mutex);
      fn = std::move(done);
      done = nullptr;
    }
    if (fn) fn(std::move(idToken), std::move(error));
  }
};

GoogleSignIn::~GoogleSignIn() { Cancel(); }

bool GoogleSignIn::Configured() {
  return std::string(config::kGoogleOAuthClientId).size() > 0;
}

void GoogleSignIn::Cancel() {
  auto attempt = attempt_;
  attempt_.reset();
  if (!attempt) return;
  attempt->cancelled = true;
  attempt->Close();
  // Drop the callback without invoking it: the caller asked to abandon this.
  std::scoped_lock lock(attempt->mutex);
  attempt->done = nullptr;
}

void GoogleSignIn::Start(Done done) {
  Cancel();  // one round trip at a time
  if (!Configured()) {
    if (done) done("", "this build has no Google OAuth client id");
    return;
  }

  auto attempt = std::make_shared<Attempt>();
  attempt->done = std::move(done);
  attempt_ = attempt;

  // WSAStartup is idempotent per process and the SDK/service already use
  // Winsock, but this class must not depend on somebody else having called it.
  WSADATA wsa{};
  ::WSAStartup(MAKEWORD(2, 2), &wsa);

  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    attempt->Finish("", "could not open the loopback callback socket");
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;  // an ephemeral port; the redirect uri carries whichever we get
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  int addrLen = sizeof(addr);
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
      ::listen(listener, 1) == SOCKET_ERROR ||
      ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
    ::closesocket(listener);
    attempt->Finish("", "could not bind the loopback callback socket");
    return;
  }
  const int port = ::ntohs(addr.sin_port);
  {
    std::scoped_lock lock(attempt->mutex);
    attempt->listener = listener;
  }

  const auto verifierBytes = RandomBytes(32);
  const auto stateBytes = RandomBytes(16);
  if (verifierBytes.empty() || stateBytes.empty()) {
    attempt->Close();
    attempt->Finish("", "the system random number generator is unavailable");
    return;
  }
  const std::string verifier = Base64Url(verifierBytes.data(), verifierBytes.size());
  const std::string state = Base64Url(stateBytes.data(), stateBytes.size());
  const auto challengeBytes = Sha256(verifier);
  if (challengeBytes.empty()) {
    attempt->Close();
    attempt->Finish("", "the system hash provider is unavailable");
    return;
  }
  const std::string challenge = Base64Url(challengeBytes.data(), challengeBytes.size());
  const std::string redirectUri = "http://127.0.0.1:" + std::to_string(port);

  const std::string authUrl = std::string(kAuthEndpoint) +
                              "?response_type=code&client_id=" + Esc(config::kGoogleOAuthClientId) +
                              "&redirect_uri=" + Esc(redirectUri) + "&scope=" + Esc(kScope) +
                              "&state=" + Esc(state) + "&code_challenge=" + Esc(challenge) +
                              "&code_challenge_method=S256&access_type=offline&prompt=select_account";

  LogInfo("google-signin: waiting for the browser on 127.0.0.1:{}", port);
  OpenBrowser(authUrl);

  std::thread([attempt, listener, verifier, state, redirectUri] {
    // Bound the wait: a user who abandons the browser tab must not leave a
    // thread and a listening socket alive for the life of the process.
    DWORD timeout = kAcceptTimeoutMs;
    ::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval tv{static_cast<long>(kAcceptTimeoutMs / 1000), 0};
    const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
    if (ready <= 0) {
      attempt->Close();
      if (!attempt->cancelled) attempt->Finish("", "the Google sign-in timed out");
      return;
    }

    SOCKET client = ::accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      attempt->Close();
      // A cancel closed the socket out from under us; that is not an error.
      if (!attempt->cancelled) attempt->Finish("", "the loopback callback was interrupted");
      return;
    }

    // The request line is all we need and it always arrives in the first
    // segment; read one buffer and stop.
    char buffer[4096];
    const int n = ::recv(client, buffer, sizeof(buffer) - 1, 0);
    std::string requestLine;
    if (n > 0) {
      buffer[n] = '\0';
      const std::string request(buffer, static_cast<size_t>(n));
      requestLine = request.substr(0, request.find("\r\n"));
    }
    ::send(client, kDonePage, static_cast<int>(std::strlen(kDonePage)), 0);
    ::shutdown(client, SD_BOTH);
    ::closesocket(client);
    attempt->Close();
    if (attempt->cancelled) return;

    const std::string oauthError = QueryParam(requestLine, "error");
    if (!oauthError.empty()) {
      attempt->Finish("", oauthError);
      return;
    }
    const std::string returnedState = QueryParam(requestLine, "state");
    if (returnedState != state) {
      // CSRF guard: a callback we did not initiate. Refuse it loudly rather
      // than exchanging a code that may not be ours.
      attempt->Finish("", "the Google sign-in callback did not match this request");
      return;
    }
    const std::string code = QueryParam(requestLine, "code");
    if (code.empty()) {
      attempt->Finish("", "the Google sign-in callback carried no code");
      return;
    }

    std::string error;
    const std::string idToken = ExchangeCode(code, verifier, redirectUri, error);
    if (attempt->cancelled) return;
    attempt->Finish(idToken, idToken.empty() ? (error.empty() ? "no id token" : error)
                                             : std::string());
  }).detach();
}

}  // namespace urnw
