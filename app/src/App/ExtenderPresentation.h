// Everything the extender surfaces decide BEFORE they touch a XAML object
// (connect/EXTENDER.md E7, K4, K6, K7, N7, O8): the gossip status dot's colour
// and word, the panel's "N of M" and event rate, the settings form's text and
// placeholders, whether the form's reset is live, the bootstrap DNS-over-HTTPS
// servers box and its messages, the share screen's QR placement, whether an
// import may proceed, the provider extender row's dot, text and switch, the
// router port mapping switch's status line, and which of the Earnings page's
// provider and extender statistics sections show.
//
// It is all here, and it is all pure, for the reason IpFamilyStatus.h gives:
// the windows solution has no test project and a WinUI 3 app cannot even be
// built on the machine this was written on, so anything expressed as a
// function of plain values is verified by tools/extender-tests.cpp on any
// host, and only the drawing is unverified. The SDK structs are mirrored here
// as plain views rather than included, so the tests need neither
// urnetwork_sdk.hpp nor the SDK dll; the WinUI layer copies the three or four
// fields across at its boundary (ExtenderPanel.cpp, ExtenderSheets.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ExtenderRingGeometry.h"

namespace urnw {

// ---- the gossip status dot (K4) --------------------------------------------

// green connected, yellow connecting, red disconnected. The SDK derives the
// state for both roles (feed and member) so every app draws one value; this
// only chooses the paint and the word.
enum class GossipTone { Green, Yellow, Red };

// The SDK's GossipState tokens. Mirrors URNET_EXTENDER_GOSSIP_STATE_* so a
// build that has not seen the SDK header still agrees with it.
inline constexpr const char* kGossipStateConnected = "connected";
inline constexpr const char* kGossipStateConnecting = "connecting";
inline constexpr const char* kGossipStateDisconnected = "disconnected";

// An unknown token is RED, never green: "I do not recognise this state" and
// "the network is up" are different answers, and only one of them is safe to
// show as reassurance.
GossipTone ExtenderGossipToneFor(std::string_view gossipState);

// The localization key for the state word. Note the store spells the middle
// one `gossip_connecting`, not `connecting`: `connected` and `disconnected`
// are the shared connection words, the yellow one is the gossip network's own.
const char* ExtenderGossipStateKey(std::string_view gossipState);

// ---- the panel's model (K4) -------------------------------------------------

// As much of the SDK's ExtenderInfo as the panel reads.
struct ExtenderInfoView {
  std::string ip;
  std::string colorHex;
  // addresses carrying at least one live connection right now
  std::int64_t inUse = 0;

  bool operator==(const ExtenderInfoView& o) const {
    return ip == o.ip && colorHex == o.colorHex && inUse == o.inUse;
  }
  bool operator!=(const ExtenderInfoView& o) const { return !(*this == o); }
};

// As much of the SDK's ExtenderStatus as the panel reads.
struct ExtenderStatusView {
  std::string gossipState;
  // K5: N is the addresses carrying a live connection, M every usable entry
  // (known, key active, not on hold) -- what the strategy would dial.
  std::int64_t activeCount = 0;
  std::int64_t reserveCount = 0;
  std::int64_t eventCountLastMinute = 0;
  std::vector<ExtenderInfoView> extenders;

  // The SDK fires once a second whether or not anything moved, so the feed
  // dedups on this before it reaches the UI thread.
  bool operator==(const ExtenderStatusView& o) const {
    return gossipState == o.gossipState && activeCount == o.activeCount &&
           reserveCount == o.reserveCount &&
           eventCountLastMinute == o.eventCountLastMinute && extenders == o.extenders;
  }
  bool operator!=(const ExtenderStatusView& o) const { return !(*this == o); }
};

// What the panel draws, with nothing left to decide.
struct ExtenderPanelModel {
  GossipTone tone = GossipTone::Red;
  // a key for Loc(), never a rendered word
  std::string stateKey = kGossipStateDisconnected;
  std::int64_t activeCount = 0;
  std::int64_t reserveCount = 0;
  std::int64_t eventCountLastMinute = 0;
  // one hollow ring per ACTIVE extender, in status order, in its colour
  std::vector<ExtenderMark> activeMarks;

  bool operator==(const ExtenderPanelModel& o) const {
    return tone == o.tone && stateKey == o.stateKey && activeCount == o.activeCount &&
           reserveCount == o.reserveCount &&
           eventCountLastMinute == o.eventCountLastMinute && activeMarks == o.activeMarks;
  }
  bool operator!=(const ExtenderPanelModel& o) const { return !(*this == o); }
};

ExtenderPanelModel ExtenderPanelModelFor(const ExtenderStatusView& status);

// ---- the settings form (K6) -------------------------------------------------

// As much of the SDK's ExtenderSettings as the form reads. `dnsName` and
// `gossipUrl` are the EFFECTIVE values; the `...Default` flags say whether
// that value is the derived default rather than something configured.
struct ExtenderSettingsView {
  std::string dnsName;
  bool dnsNameDefault = false;
  std::string gossipUrl;
  bool gossipUrlDefault = false;
  std::vector<std::string> hosts;
  std::string networkHost;
  std::vector<std::string> rootPublicKeys;
  bool rootPublicKeysDefault = false;
};

// The three fields, as text. An empty box MEANS the default (that is the SDK's
// contract for SetSettings), so a value that IS the default is shown as an
// empty box with the default named in the placeholder -- clearing a box is how
// a user goes back to the default, and a box pre-filled with the default would
// turn every save into an explicit override of it.
struct ExtenderSettingsForm {
  std::string dnsNameText;
  // the argument for `extender_default_value`; empty when there is no default
  // to name (the value is configured, so the placeholder is never seen)
  std::string dnsNameDefaultValue;
  std::string gossipUrlText;
  std::string gossipUrlDefaultValue;
  // one host per line, in the configured order
  std::string hostsText;

  bool operator==(const ExtenderSettingsForm& o) const {
    return dnsNameText == o.dnsNameText && dnsNameDefaultValue == o.dnsNameDefaultValue &&
           gossipUrlText == o.gossipUrlText &&
           gossipUrlDefaultValue == o.gossipUrlDefaultValue && hostsText == o.hostsText;
  }
  bool operator!=(const ExtenderSettingsForm& o) const { return !(*this == o); }
};

ExtenderSettingsForm ExtenderSettingsFormFor(const ExtenderSettingsView& settings);

// The view of the SDK's ExtenderSettings, copied across by the generated
// wrapper's field names (urnet::ExtenderSettings) so this header still needs
// no SDK header: what the form's load reads, and its reload after a reset.
template <class Settings>
ExtenderSettingsView ExtenderSettingsViewOf(const Settings& settings) {
  ExtenderSettingsView view;
  view.dnsName = settings.DnsName;
  view.dnsNameDefault = settings.DnsNameDefault;
  view.gossipUrl = settings.GossipUrl;
  view.gossipUrlDefault = settings.GossipUrlDefault;
  view.networkHost = settings.NetworkHost;
  if (settings.Hosts) view.hosts = *settings.Hosts;
  if (settings.RootPublicKeys) view.rootPublicKeys = *settings.RootPublicKeys;
  view.rootPublicKeysDefault = settings.RootPublicKeysDefault;
  return view;
}

// The hosts box back into the list SetSettings takes: split on newlines (and
// commas, because a user who has a comma-separated list in hand will paste
// it), trimmed, empties dropped, duplicates kept in order -- the SDK owns the
// policy, this owns only the typing.
std::vector<std::string> ParseExtenderHostLines(std::string_view text);

// ---- reset (E7) -------------------------------------------------------------
//
// "Reset extenders" returns this installation's extender state to a fresh
// install's: what was learned is cleared and the extenders a user added are
// removed, in the app's own space and then in the service's
// (SdkHost::ResetExtenders). The pane confirms first and reloads the form
// after, which then shows every setting at its default. The reset's spec,
// ExtenderSettingsViewOf's with it, runs in tools/reset-extenders-tests.cpp
// beside the control verb's.

// Whether the pane's Reset extenders button is live. Unlike share and import
// it needs no session: the reset is of the app's own space, and the service
// takes it by its control verb or at its next import of the space. It does
// need a signed-in account (the pane is the account's), and it waits while
// another write of the pane is running, so a save and a reset never cross.
bool ExtenderResetEnabled(bool signedIn, bool writing);

// ---- the bootstrap DNS-over-HTTPS servers (sdk control_doh_ui.go) -----------
//
// `https://<ip literal>/<path>` servers the space's own names (api, connect,
// extender) are looked up through ahead of the built-in ones, for networks that
// block those. They are a network-space value, so they are read and saved
// through SdkHost against the app's own space, with or without a session.

// The SDK's error ids (urnet::ControlDohError*), mirrored so a build that has
// not seen the SDK header still agrees with it. Each is the store key of its
// message, the vless_error_* pattern.
inline constexpr const char* kControlDohErrorUrlInvalid = "control_doh_error_url_invalid";
inline constexpr const char* kControlDohErrorHttpsRequired = "control_doh_error_https_required";
inline constexpr const char* kControlDohErrorIpRequired = "control_doh_error_ip_required";
inline constexpr const char* kControlDohErrorTooMany = "control_doh_error_too_many";

inline constexpr const char* kControlDohErrorIds[] = {
    kControlDohErrorUrlInvalid,
    kControlDohErrorHttpsRequired,
    kControlDohErrorIpRequired,
    kControlDohErrorTooMany,
};

// The country whose preset "Use China resolvers" asks the SDK for
// (urnet::regionalControlDohUrls), the one source of the preset's servers.
inline constexpr const char* kControlDohChinaCountryCode = "cn";

// The servers box back into the list setControlDohUrls takes: split on line
// breaks (\n and \r), trimmed, blank lines dropped, in order. Not on commas, as
// the hosts box does: a url may carry one in its path, and every platform
// splits this box the same way. Repeats are kept; the SDK drops them and
// normalizes the rest.
std::vector<std::string> ParseControlDohLines(std::string_view text);

// The servers as the box shows them: one per line.
std::string ControlDohText(const std::vector<std::string>& urls);

// The store key for an error id from setControlDohUrls: the id itself for the
// four the sdk names, and something_went_wrong for anything else -- an id this
// build does not know (a newer SDK) and the sdk's answer for a call that could
// not run (kSdkErrorIdInternal, SdkErrorId.h) alike -- never the bare id on
// screen.
const char* ControlDohErrorKey(std::string_view errorId);

// What the block says for the sdk's answer to setControlDohUrls. Only "" is
// saved, with the next-connect note (the service imports the space at its next
// StartTunnel); any other answer saved nothing, and its message is
// ControlDohErrorKey's.
struct ControlDohSaveOutcome {
  bool saved = false;
  // a store key, never an sdk id
  const char* messageKey = "";
  bool nextConnectNote = false;
};

ControlDohSaveOutcome ControlDohSaveOutcomeFor(std::string_view errorId);

// ---- share (K7) -------------------------------------------------------------

inline constexpr const char* kExtenderSharePrefix = "ur-ext:1:";

// Trim a pasted payload: a share arrives out of a chat window or an email and
// carries whatever whitespace came with it.
std::string TrimShareText(std::string_view text);

// A cheap shape check for enabling the import button, NOT a decoder -- the SDK
// is the only thing that decides whether a payload is real (DecodeShare).
bool LooksLikeExtenderShare(std::string_view text);

// Where the connector glyph sits on the rendered code (K7: "the QR renders at
// error level H with the black and white connector glyph centered and a 4 px
// outline of the connector shape around it").
//
// Level H recovers up to ~30% of the codewords, so a centred occlusion of a
// few percent of the area is well inside the budget; kExtenderQrGlyphFraction
// is the glyph's side as a fraction of the code's side, and the OUTLINE is
// counted into the cleared area too -- clearing only the glyph would leave the
// outline painted over live modules, where its 4 px would not read as an
// outline at all.
struct ExtenderQrLayout {
  int moduleCount = 0;     // the code's side, in modules
  double moduleSize = 0;   // px per module
  double side = 0;         // the drawn code's side in px (moduleCount * moduleSize)
  double glyphSide = 0;    // the connector glyph's box
  double glyphLeft = 0;    // relative to the code's top-left
  double glyphTop = 0;
  double outlineThickness = 0;
  // The module index range blanked under the glyph, [clearFrom, clearTo). Both
  // axes, since the cleared area is square and centred.
  int clearFrom = 0;
  int clearTo = 0;
};

inline constexpr double kExtenderQrGlyphFraction = 0.22;
inline constexpr double kExtenderQrOutline = 4.0;

// `pixelSide` is the space the code is drawn into. A moduleCount of zero (no
// code) or a non-positive side yields a zeroed layout, which draws nothing.
ExtenderQrLayout ExtenderQrLayoutFor(int moduleCount, double pixelSide);

// One horizontal run of dark modules, in module coordinates. The code is drawn
// as rectangles, and a 37-module code is ~680 dark modules: merging each row's
// runs turns that into ~200 shapes, which is the difference between a dialog
// that opens and one that hitches while it does.
struct ExtenderQrRun {
  int x = 0;
  int y = 0;
  int length = 0;

  bool operator==(const ExtenderQrRun& o) const {
    return x == o.x && y == o.y && length == o.length;
  }
};

// The dark runs of a code, with the modules under the glyph and its outline
// BLANKED (K7). `dark` is row-major, moduleCount * moduleCount; the clear range
// is [clearFrom, clearTo) on both axes, as ExtenderQrLayoutFor reports it.
// Blanking here rather than at draw time is what keeps the glyph's white box
// from being drawn over live modules, where the 4 px outline would not read as
// an outline at all.
std::vector<ExtenderQrRun> ExtenderQrRunsFor(int moduleCount, const std::vector<bool>& dark,
                                             int clearFrom, int clearTo);

// ---- import (K7, K8) ---------------------------------------------------------

// The SDK's ExtenderShareDecodeResult. `error` is a KEY ID
// (`import_extenders_invalid`, `import_extenders_foreign_host`), not a
// sentence, so every app renders the same words in its own language.
struct ExtenderShareDecodeView {
  bool ok = false;
  std::string error;
  std::string networkHost;
  bool foreignHost = false;
  std::int64_t count = 0;
  bool hasSettings = false;
  std::string settingsHost;
  // the bootstrap DNS-over-HTTPS servers the settings block names, v4 then v6;
  // empty when it names none
  std::vector<std::string> controlDohUrls;
};

// What the import sheet may do with a decoded payload and the current state of
// its "use extender settings" toggle.
struct ExtenderImportDecision {
  // the Import button is live
  bool canImport = false;
  // the payload carries a settings block, so the toggle is shown at all
  bool showSettingsToggle = false;
  // the payload is for another operator: say so, whether or not it blocks
  bool showForeignHost = false;
  // importing would replace the dns name, gossip url and root keys, so it
  // takes a confirmation first
  bool needsConfirm = false;
  // "" when there is nothing to say; otherwise a store key
  std::string messageKey;
  // the argument of messageKey (a host), empty when it takes none
  std::string messageArg;
  // the argument of `import_extenders_confirm_settings`
  std::string confirmArg;
  // the argument of `import_extenders_control_doh_urls`: the bootstrap
  // DNS-over-HTTPS servers the settings block names, joined with ", ". Empty,
  // and the line hidden, when the code carries no settings block or its block
  // names none. The servers are taken only with the settings, and saying so
  // before the toggle is the point: they will see URnetwork's lookups.
  std::string controlDohUrlsArg;
};

// K7: "An import whose network host differs from the space's is refused unless
// 'use extender settings' is chosen, which shows the operator host and asks to
// confirm before replacing the dns name, gossip url and root keys."
//
// So a foreign payload is NOT an error to be dismissed -- it is an import the
// user can still choose, by taking the settings with it. The message stays on
// screen either way, because a code that quietly imported nothing would be
// indistinguishable from one that worked.
ExtenderImportDecision DecideExtenderImport(const ExtenderShareDecodeView& decoded,
                                            bool useSettings);

// The SDK's ExtenderImportResult.
struct ExtenderImportResultView {
  bool ok = false;
  std::string error;  // a key id, as above
  std::int64_t importedCount = 0;
};

// What to say after an import ran: a plural key plus its count on success, the
// error key otherwise. `isPlural` says which of Loc / Plural to call.
struct ExtenderImportOutcome {
  bool ok = false;
  std::string messageKey;
  bool isPlural = false;
  std::int64_t count = 0;
};

ExtenderImportOutcome ExtenderImportOutcomeFor(const ExtenderImportResultView& result);

// ---- the provider extender row (N7) -------------------------------------------

// The SDK's ExtenderProvideState tokens and ExtenderProvideError cases. Mirrors
// urnet::ExtenderProvideState* and urnet::ExtenderProvideError* so a build that
// has not seen the SDK header still agrees with it, as the gossip tokens do.
inline constexpr const char* kExtenderProvideStateOff = "off";
inline constexpr const char* kExtenderProvideStateNotProviding = "not_providing";
inline constexpr const char* kExtenderProvideStateSettingUp = "setting_up";
inline constexpr const char* kExtenderProvideStateActive = "active";
inline constexpr const char* kExtenderProvideStateError = "error";
inline constexpr const char* kExtenderProvideErrorRevoked = "revoked";
inline constexpr const char* kExtenderProvideErrorStart = "start";
// tcp 443, the one port the role requires, could not be bound (usually another
// program holds it): the role stays off and the SDK binds again every few minutes
inline constexpr const char* kExtenderProvideErrorTcpUnavailable = "tcp_unavailable";
inline constexpr const char* kExtenderProvideErrorListen = "listen";
inline constexpr const char* kExtenderProvideErrorActivationFailed = "activation_failed";
inline constexpr const char* kExtenderProvideErrorActivationRefused = "activation_refused";
inline constexpr const char* kExtenderProvideErrorActivationUnreachable = "activation_unreachable";

// The SDK's ExtenderPortMapping tokens, the router port mapping's state in the
// status (connect EXTENDER.md S10), empty while no role runs. Mirrors
// urnet::ExtenderPortMapping* as the tokens above mirror theirs.
inline constexpr const char* kExtenderPortMappingOff = "off";
inline constexpr const char* kExtenderPortMappingNotNeeded = "not_needed";
inline constexpr const char* kExtenderPortMappingMapping = "mapping";
inline constexpr const char* kExtenderPortMappingMapped = "mapped";
inline constexpr const char* kExtenderPortMappingFailed = "failed";

// As much of the SDK's ExtenderProvideStatus as the apps read (N7), with the
// setting read beside it. The SDK derived `state` and `errorCase` once from the
// fields this leaves out (RevokedTime, Listening, ListenError, StartError,
// TcpUnavailableError, LastActivationError), so nothing here re-derives that
// rule. Of the router port mapping's reading (S10) it holds the state and the
// two texts an app may show, never PortMappingError or PortMappingExternalIp:
// the router's words can name an address, and they stay in the device's log.
//
//   refused          LastActivationRefused: in the active state, whether
//                    `reason` (the other family's text) is a refusal
//   enabled          the role is running; the statistics sections read it
//                    (O8), the row does not
//   provideExtender  Device::getProvideExtender() read beside each status whose
//                    row shows: the switch's position. The DeviceRemote answers
//                    the queued or last-known value while the device process is
//                    out of contact, so the switch holds through a daemon
//                    restart.
//   providerOnly     the status is the service's provider-only device's, the
//                    provider while there is no session (get_provider_stats),
//                    not a session's device's. The app has no device rpc to
//                    that device: SdkHost::SetProvideExtender writes its
//                    setting through the service instead (set_provide_extender).
//   serviceWritable  providerOnly only: the service said it takes that write
//                    (get_provider_stats' provide_extender_writable). A service
//                    from before the verb does not, and the switch stays
//                    hidden over its status, never a dead switch (N1).
//   measurementsEnabled
//                    Device::getMeasurementsEnabled() read beside the provide
//                    extender setting: the lag.dev speed tests switch's
//                    position (lag.dev/PLAN.md C4). On by default, as the sdk's.
//   measurementsServiceWritable
//                    providerOnly only: the service said it takes the speed
//                    tests switch's write too (get_provider_stats'
//                    measurements_enabled_writable). A service from before
//                    set_measurements_enabled does not, and that switch stays
//                    hidden over its status (N1).
//   provideExtenderPortMapping
//                    Device::getProvideExtenderPortMapping() read beside the
//                    provide extender setting: the router port mapping switch's
//                    position ("Open a port on my router", connect EXTENDER.md
//                    S8). Off by default, as the sdk's on every app.
//   portMappingServiceWritable
//                    providerOnly only: the service said it takes the router
//                    port mapping switch's write too (get_provider_stats'
//                    provide_extender_port_mapping_writable). A service from
//                    before set_provide_extender_port_mapping does not, and
//                    that switch stays hidden over its status (N1).
//   portMapping      PortMapping: the router port mapping's state as the role
//                    reports it (kExtenderPortMapping*), empty while no role
//                    runs. The role's report, not the switch's position.
//   portMappingMethodName, portMappingPorts
//                    PortMappingMethodName ("PCP", "NAT-PMP", "UPnP") and
//                    PortMappingPorts ("TCP 443, UDP 443, UDP 4053", or
//                    "TCP 40443→443" for a port the router moved): how the
//                    router was asked and what it forwards, the same in every
//                    language, empty unless mapped. The status line's two
//                    texts.
//
// A default-constructed view is "no session": unsupported, so both rows hide.
struct ExtenderProvideStatusView {
  bool supported = false;
  std::string state = kExtenderProvideStateOff;
  std::string errorCase;
  std::string reason;
  bool activatedV4 = false;
  bool activatedV6 = false;
  bool refused = false;
  bool enabled = false;
  bool provideExtender = false;
  bool providerOnly = false;
  bool serviceWritable = false;
  bool measurementsEnabled = true;
  bool measurementsServiceWritable = false;
  bool provideExtenderPortMapping = false;
  bool portMappingServiceWritable = false;
  std::string portMapping;
  std::string portMappingMethodName;
  std::string portMappingPorts;

  // The device pushes a status after any change of the setting, the provide
  // state or the role, coalesced to one status per epoch and never on
  // registration. SdkHost keeps a push equal to the last one off the UI thread
  // on this comparison, so it covers every field the row and the sections read,
  // and which device the status is of and whether its switch can write it: a
  // session that takes over from the provider-only device with the same
  // reading still republishes.
  bool operator==(const ExtenderProvideStatusView& o) const {
    return supported == o.supported && state == o.state && errorCase == o.errorCase &&
           reason == o.reason && activatedV4 == o.activatedV4 &&
           activatedV6 == o.activatedV6 && refused == o.refused && enabled == o.enabled &&
           provideExtender == o.provideExtender && providerOnly == o.providerOnly &&
           measurementsEnabled == o.measurementsEnabled &&
           measurementsServiceWritable == o.measurementsServiceWritable &&
           provideExtenderPortMapping == o.provideExtenderPortMapping &&
           portMappingServiceWritable == o.portMappingServiceWritable &&
           portMapping == o.portMapping &&
           portMappingMethodName == o.portMappingMethodName &&
           portMappingPorts == o.portMappingPorts &&
           serviceWritable == o.serviceWritable;
  }
  bool operator!=(const ExtenderProvideStatusView& o) const { return !(*this == o); }
};

// The view of the SDK's ExtenderProvideStatus (N7), read from the fields the
// apps may read and nothing else, with the settings read beside it only when
// the row will show: a device that reports the role unsupported is never asked
// for a setting it may not have (N1). Those are the provide extender setting
// and the two that ride on the role, the lag.dev measurements setting and the
// router port mapping setting. Generic over the status type so the host tests
// hand it every field of urnet::ExtenderProvideStatus by the SDK's names. A
// session's device's status and the provider-only device's (the service's
// document, Protocol.h ExtenderProvideStatusOf) both come through here, so the
// rows and the port mapping line read the same on either.
template <typename Status, typename ReadSetting, typename ReadMeasurements,
          typename ReadPortMapping>
ExtenderProvideStatusView ExtenderProvideStatusViewOf(const std::optional<Status>& status,
                                                      const ReadSetting& readSetting,
                                                      const ReadMeasurements& readMeasurements,
                                                      const ReadPortMapping& readPortMapping) {
  ExtenderProvideStatusView view;
  if (!status || !status->Supported) return view;
  view.supported = true;
  view.state = status->State;
  view.errorCase = status->ErrorCase;
  view.reason = status->Reason;
  view.activatedV4 = status->ActivatedV4;
  view.activatedV6 = status->ActivatedV6;
  view.refused = status->LastActivationRefused;
  view.enabled = status->Enabled;
  // the router port mapping's state and the two texts the line may show (S10),
  // never PortMappingError or PortMappingExternalIp
  view.portMapping = status->PortMapping;
  view.portMappingMethodName = status->PortMappingMethodName;
  view.portMappingPorts = status->PortMappingPorts;
  view.provideExtender = readSetting();
  view.measurementsEnabled = readMeasurements();
  view.provideExtenderPortMapping = readPortMapping();
  return view;
}

// The dot, by state: grey off and not providing, yellow setting up, green
// active, red error. The WinUI layer picks the colours (ProvideModeVisual.h).
enum class ExtenderProvideTone { Grey, Green, Yellow, Red };

// How the state key takes its "{}".
enum class ExtenderProvideArgument {
  None,  // the key is the whole text: off, not providing, setting up, revoked
  Text,  // the argument is the SDK's raw Reason, filled in as it is
  Key,   // the argument is a store key, localized first: the families of active
};

// What the extender row draws, with nothing left to decide (N7). Store keys,
// never rendered words; ComposeExtenderProvideText makes them the one line.
struct ExtenderProvideRowModel {
  // The row, its description and its switch show at all: the view's
  // `supported`. Hidden, never disabled (N1).
  bool visible = false;
  // The Connect page's row, the one with the switch, shows: `visible`, and the
  // switch's write has a taker — a session's device, over its device rpc, or
  // with no session the service, for the provider-only device's status
  // (`serviceWritable`). Over a provider-only status a service from before
  // set_provide_extender cannot take, that row stays hidden, never a dead
  // switch (N1), while the Earnings row, which has no switch, reads `visible`
  // and shows it.
  bool switchVisible = false;
  ExtenderProvideTone tone = ExtenderProvideTone::Grey;
  // The state key. Empty when `argument` renders bare: an error case this
  // build does not know (a newer device process), or a state it does not know.
  std::string textKey = "off";
  std::string argument;
  ExtenderProvideArgument argumentKind = ExtenderProvideArgument::None;
  // Active only: the other family's last attempt failed, so its text follows
  // on the same line after " · ": this key, filled with `suffixArgument` (the
  // Reason). Empty when there is nothing to add.
  std::string suffixKey;
  std::string suffixArgument;
  // the switch's position
  bool on = false;

  bool operator==(const ExtenderProvideRowModel& o) const {
    return visible == o.visible && switchVisible == o.switchVisible && tone == o.tone &&
           textKey == o.textKey && argument == o.argument && argumentKind == o.argumentKind &&
           suffixKey == o.suffixKey && suffixArgument == o.suffixArgument && on == o.on;
  }
  bool operator!=(const ExtenderProvideRowModel& o) const { return !(*this == o); }
};

// N7's reading, from `state` and, in the error state, from `errorCase` alone:
//
//   off            grey    off
//   not_providing  grey    extender_not_providing
//   setting_up     yellow  extender_setting_up
//   active         green   extender_active with the families (ipv4_and_ipv6
//                          when both hold, else ipv4 or ipv6); with a Reason,
//                          " · " and extender_activation_refused (refused) or
//                          extender_activation_failed, filled with the Reason
//   error          red     revoked: extender_revoked; start, tcp_unavailable,
//                          listen, activation_refused, activation_failed: their
//                          key filled with the Reason (for tcp_unavailable the
//                          bind error); any other case: the Reason bare
//
// A state this build does not know is grey with the Reason bare: it names no
// case the SDK did not pick, and claims no error the SDK did not report.
ExtenderProvideRowModel ExtenderProvideRowModelFor(const ExtenderProvideStatusView& view);

// The row the switch paints the moment it is flipped, before the listener
// answers (N7): off is grey Off; on is yellow Setting up while the device is
// providing and grey Not providing while it is not. The view comes back with
// the state and the setting guessed and the rest of the SDK's reading cleared,
// still of the device it was read from and with the same writer, so it draws
// through ExtenderProvideRowModelFor like any status; the next pushed status
// replaces it.
ExtenderProvideStatusView ExtenderProvideGuessFor(const ExtenderProvideStatusView& current,
                                                  bool on, bool providing);

// Where the switch's write goes (SdkHost::SetProvideExtender): through a
// session's device whenever one is bound; with none, through the service for
// the provider-only device's status the switch was shown over, when the service
// said it takes it; otherwise nowhere — the row is hidden then (N1), so a
// write that arrives anyway is dropped.
enum class ExtenderProvideWriteRoute {
  Device,   // the session's DeviceRemote, over the device rpc
  Service,  // set_provide_extender, off the UI thread
  None,
};

ExtenderProvideWriteRoute ExtenderProvideWriteRouteFor(bool sessionDevice,
                                                       const ExtenderProvideStatusView& shown);

// ---- the lag.dev speed tests switch (lag.dev/PLAN.md C4, decision 18) --------

// The switch under the Extender row: whether this device's extender role
// serves lag.dev's measurement API, so people testing their connection there
// can measure to it. It rides on the role, so it shows exactly while the
// Extender switch does, and only where its own write has a taker: a session's
// device, or for the provider-only device's status the service, when it said
// it takes this write too (measurementsServiceWritable). Hidden, never
// disabled (N1).
struct SpeedTestsRowModel {
  bool visible = false;
  // the setting: on by default, as the sdk's
  bool on = true;

  bool operator==(const SpeedTestsRowModel& o) const {
    return visible == o.visible && on == o.on;
  }
  bool operator!=(const SpeedTestsRowModel& o) const { return !(*this == o); }
};

SpeedTestsRowModel SpeedTestsRowModelFor(const ExtenderProvideStatusView& view);

// Where the speed tests switch's write goes (SdkHost::SetMeasurementsEnabled):
// the Extender switch's rule, with the service's own say for this setting. So
// the switch is written exactly where it shows.
ExtenderProvideWriteRoute SpeedTestsWriteRouteFor(bool sessionDevice,
                                                  const ExtenderProvideStatusView& shown);

// The view the speed tests switch paints the moment it is flipped: the shown
// view with the setting guessed and nothing else changed. The next pushed
// status replaces it, as it does the Extender switch's guess.
ExtenderProvideStatusView SpeedTestsGuessFor(const ExtenderProvideStatusView& current, bool on);

// ---- the router port mapping switch (connect EXTENDER.md S8) -----------------

// The switch under the speed tests switch, "Open a port on my router": whether
// this device's extender role asks the router, by UPnP, NAT-PMP or PCP, to
// forward its ports (tcp 443, udp 443 and udp 4053) to this device, so the
// extender can be reached from the internet. Turning it on opens those ports on
// the router, so it is off until the user turns it on, on every app (owner,
// 2026-11-11; the miner turns it on). It rides on the role, so it shows only
// where the speed tests switch does, and only where its own write has a taker:
// a session's device, or for the provider-only device's status the service,
// when it said it takes this write too (portMappingServiceWritable). Hidden,
// never disabled (N1).
//
// Under its description, one status line (S10) while the switch shows and is
// on and the role reports a state: what the router did. The sdk's state reads
//
//   not_needed  extender_port_mapping_not_needed (a public address)
//   mapping     extender_port_mapping_mapping (asking the router)
//   mapped      extender_port_mapping_mapped, its {0} the ports text and its
//               {1} the method name, the sdk's two texts; no line when either
//               is empty, rather than a sentence with empty parentheses
//   failed      extender_port_mapping_failed (the device asks again itself)
//
// and off, no role (empty) or a state this build does not know read as no
// line: none of them says what the router did. Never the router's own words
// (PortMappingError), which can name an address: the view does not hold them.
struct PortMappingRowModel {
  bool visible = false;
  // the setting: off by default, as the sdk's on every app
  bool on = false;
  // the status line's store key; empty for no line
  std::string statusKey;
  // the mapped line's {0} and {1}, the sdk's texts as they are; empty for a
  // line that is its key's whole text
  std::string statusPorts;
  std::string statusMethod;

  bool operator==(const PortMappingRowModel& o) const {
    return visible == o.visible && on == o.on && statusKey == o.statusKey &&
           statusPorts == o.statusPorts && statusMethod == o.statusMethod;
  }
  bool operator!=(const PortMappingRowModel& o) const { return !(*this == o); }
};

PortMappingRowModel PortMappingRowModelFor(const ExtenderProvideStatusView& view);

// The status line as `Text`, empty for none. `localized(key)` answers the
// store's text for a key, `formatted(key, ports, method)` the store's text with
// its {0} and {1} filled, which a translation may reorder, and `fromUtf8` lifts
// the sdk's texts into `Text`. A template so the composition is checked off
// Windows with plain strings; the app passes Localized, Format and Widen
// (ProvideModeVisual.h).
template <typename Text, typename LocalizedFn, typename FormattedFn, typename FromUtf8Fn>
Text ComposePortMappingStatusText(const PortMappingRowModel& model, LocalizedFn&& localized,
                                  FormattedFn&& formatted, FromUtf8Fn&& fromUtf8) {
  if (model.statusKey.empty()) return Text{};
  // the mapped line, the one that takes the sdk's two texts
  if (!model.statusPorts.empty()) {
    return formatted(model.statusKey, fromUtf8(model.statusPorts), fromUtf8(model.statusMethod));
  }
  return localized(model.statusKey);
}

// Where the port mapping switch's write goes
// (SdkHost::SetProvideExtenderPortMapping): the speed tests switch's rule, with
// the service's own say for this setting. So the switch is written exactly
// where it shows.
ExtenderProvideWriteRoute PortMappingWriteRouteFor(bool sessionDevice,
                                                   const ExtenderProvideStatusView& shown);

// The view the port mapping switch paints the moment it is flipped: the shown
// view with the setting guessed and the role's port mapping report dropped,
// nothing else changed. A changed setting replaces a running role (S10), so
// what the router did for the old one no longer holds, and the line waits for
// the next pushed status, which replaces the guess as it does the other
// switches' guesses.
ExtenderProvideStatusView PortMappingGuessFor(const ExtenderProvideStatusView& current, bool on);

// The one line of state text a model reads as. `localized(key)` answers the
// store's text for a key and `formatted(key, text)` the store's text with its
// "{}" filled, both as `Text`; `fromUtf8` lifts the SDK's raw Reason into
// `Text`; `separator` is the " · " between the active line and the other
// family's failure. A template so the composition is checked off Windows with
// plain strings; the app passes Localized and Format (ProvideModeVisual.h).
template <typename Text, typename LocalizedFn, typename FormattedFn, typename FromUtf8Fn>
Text ComposeExtenderProvideText(const ExtenderProvideRowModel& model, const Text& separator,
                                LocalizedFn&& localized, FormattedFn&& formatted,
                                FromUtf8Fn&& fromUtf8) {
  Text text;
  if (model.textKey.empty()) {
    text = fromUtf8(model.argument);
  } else {
    switch (model.argumentKind) {
      case ExtenderProvideArgument::None:
        text = localized(model.textKey);
        break;
      case ExtenderProvideArgument::Text:
        text = formatted(model.textKey, fromUtf8(model.argument));
        break;
      case ExtenderProvideArgument::Key:
        text = formatted(model.textKey, localized(model.argument));
        break;
    }
  }
  if (!model.suffixKey.empty()) {
    text += separator;
    text += formatted(model.suffixKey, fromUtf8(model.suffixArgument));
  }
  return text;
}

// ---- the statistics sections (O8) ---------------------------------------------

// Which parts of the Earnings page's two statistics groups show.
struct ExtenderStatsSections {
  // the provider group's chart rows: the provide mode is not never and the
  // device reports provider packet stats
  bool providerVisible = false;
  // the whole extender group, its title and its chart: the provider rows show
  // and the role is running (the pushed status's `enabled`, never the
  // throughput tick or the point count)
  bool extenderVisible = false;
  // `providing_disabled` as the provider group header's meta label, exactly
  // while the provider rows are hidden
  bool disabledMeta = true;

  bool operator==(const ExtenderStatsSections& o) const {
    return providerVisible == o.providerVisible && extenderVisible == o.extenderVisible &&
           disabledMeta == o.disabledMeta;
  }
  bool operator!=(const ExtenderStatsSections& o) const { return !(*this == o); }
};

ExtenderStatsSections ExtenderStatsSectionsFor(bool providingEnabled, bool hasProviderStats,
                                               bool extenderRunning);

}  // namespace urnw
