// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see ExtenderPresentation.h. App.vcxproj compiles this
// with PrecompiledHeader=NotUsing, like IpFamilyStatus.cpp.
#include "ExtenderPresentation.h"

#include <cmath>

namespace urnw {
namespace {

constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
  return value;
}

}  // namespace

// ---- the gossip status dot --------------------------------------------------

GossipTone ExtenderGossipToneFor(std::string_view gossipState) {
  if (gossipState == kGossipStateConnected) return GossipTone::Green;
  if (gossipState == kGossipStateConnecting) return GossipTone::Yellow;
  // kGossipStateDisconnected, empty (no status yet) and anything this build
  // does not know
  return GossipTone::Red;
}

const char* ExtenderGossipStateKey(std::string_view gossipState) {
  switch (ExtenderGossipToneFor(gossipState)) {
    case GossipTone::Green: return "connected";
    case GossipTone::Yellow: return "gossip_connecting";
    case GossipTone::Red: break;
  }
  return "disconnected";
}

// ---- the panel's model ------------------------------------------------------

ExtenderPanelModel ExtenderPanelModelFor(const ExtenderStatusView& status) {
  ExtenderPanelModel model;
  model.tone = ExtenderGossipToneFor(status.gossipState);
  model.stateKey = ExtenderGossipStateKey(status.gossipState);
  // Counts are never rendered negative: a count is a cardinality, and a
  // negative one is a bug upstream, not a number to put in front of a user.
  model.activeCount = 0 < status.activeCount ? status.activeCount : 0;
  model.reserveCount = 0 < status.reserveCount ? status.reserveCount : 0;
  model.eventCountLastMinute =
      0 < status.eventCountLastMinute ? status.eventCountLastMinute : 0;
  for (const ExtenderInfoView& extenderInfo : status.extenders) {
    // "active means carrying at least one live connection right now" (K4)
    if (extenderInfo.inUse <= 0) continue;
    if (extenderInfo.ip.empty()) continue;
    ExtenderMark mark;
    mark.ip = extenderInfo.ip;
    mark.colorHex = extenderInfo.colorHex;
    model.activeMarks.push_back(std::move(mark));
  }
  return model;
}

// ---- the settings form ------------------------------------------------------

ExtenderSettingsForm ExtenderSettingsFormFor(const ExtenderSettingsView& settings) {
  ExtenderSettingsForm form;
  if (settings.dnsNameDefault) {
    form.dnsNameDefaultValue = settings.dnsName;
  } else {
    form.dnsNameText = settings.dnsName;
  }
  if (settings.gossipUrlDefault) {
    form.gossipUrlDefaultValue = settings.gossipUrl;
  } else {
    form.gossipUrlText = settings.gossipUrl;
  }
  for (const std::string& host : settings.hosts) {
    const std::string_view trimmed = Trim(host);
    if (trimmed.empty()) continue;
    if (!form.hostsText.empty()) form.hostsText += "\n";
    form.hostsText.append(trimmed);
  }
  return form;
}

std::vector<std::string> ParseExtenderHostLines(std::string_view text) {
  std::vector<std::string> hosts;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.size();
    for (std::size_t i = start; i < text.size(); ++i) {
      if (text[i] == '\n' || text[i] == '\r' || text[i] == ',') {
        end = i;
        break;
      }
    }
    const std::string_view item = Trim(text.substr(start, end - start));
    if (!item.empty()) hosts.emplace_back(item);
    if (end == text.size()) break;
    start = end + 1;
  }
  return hosts;
}

// ---- reset (E7) -------------------------------------------------------------

bool ExtenderResetEnabled(bool signedIn, bool writing) { return signedIn && !writing; }

// ---- the bootstrap DNS-over-HTTPS servers -------------------------------------

std::vector<std::string> ParseControlDohLines(std::string_view text) {
  std::vector<std::string> urls;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.size();
    for (std::size_t i = start; i < text.size(); ++i) {
      if (text[i] == '\n' || text[i] == '\r') {
        end = i;
        break;
      }
    }
    const std::string_view line = Trim(text.substr(start, end - start));
    if (!line.empty()) urls.emplace_back(line);
    if (end == text.size()) break;
    start = end + 1;
  }
  return urls;
}

std::string ControlDohText(const std::vector<std::string>& urls) {
  std::string text;
  for (const std::string& url : urls) {
    if (!text.empty()) text += "\n";
    text += url;
  }
  return text;
}

const char* ControlDohErrorKey(std::string_view errorId) {
  for (const char* id : kControlDohErrorIds) {
    if (errorId == id) return id;
  }
  // the call could not run, or a newer sdk refused for a reason this build
  // cannot name: neither is about the url the user typed
  return "something_went_wrong";
}

ControlDohSaveOutcome ControlDohSaveOutcomeFor(std::string_view errorId) {
  ControlDohSaveOutcome outcome;
  if (!errorId.empty()) {
    outcome.messageKey = ControlDohErrorKey(errorId);
    return outcome;
  }
  outcome.saved = true;
  outcome.messageKey = "control_doh_urls_saved";
  outcome.nextConnectNote = true;
  return outcome;
}

// ---- share ------------------------------------------------------------------

std::string TrimShareText(std::string_view text) { return std::string(Trim(text)); }

bool LooksLikeExtenderShare(std::string_view text) {
  const std::string_view trimmed = Trim(text);
  const std::string_view prefix = kExtenderSharePrefix;
  if (trimmed.size() <= prefix.size()) return false;
  return trimmed.substr(0, prefix.size()) == prefix;
}

ExtenderQrLayout ExtenderQrLayoutFor(int moduleCount, double pixelSide) {
  ExtenderQrLayout layout;
  if (moduleCount <= 0 || pixelSide <= 0) return layout;
  layout.moduleCount = moduleCount;
  // A whole number of pixels per module: a fractional one leaves seams between
  // rectangles at some scales and a decoder reading a photo of the screen has
  // to work harder than it should.
  layout.moduleSize = std::floor(pixelSide / moduleCount);
  if (layout.moduleSize < 1) layout.moduleSize = 1;
  layout.side = layout.moduleSize * moduleCount;
  layout.outlineThickness = kExtenderQrOutline;

  // The glyph plus its outline has to come out of whole modules, or the
  // cleared patch and the drawn glyph disagree at the edges.
  const double wanted = layout.side * kExtenderQrGlyphFraction;
  const double cleared = wanted + 2 * layout.outlineThickness;
  int clearedModules = static_cast<int>(std::ceil(cleared / layout.moduleSize));
  // centred: the cleared run has to have the same parity as the code, and
  // every QR version has an odd module count
  if (((clearedModules ^ moduleCount) & 1) != 0) ++clearedModules;
  if (moduleCount < clearedModules) clearedModules = moduleCount;
  if (clearedModules < 1) clearedModules = 1;

  layout.clearFrom = (moduleCount - clearedModules) / 2;
  layout.clearTo = layout.clearFrom + clearedModules;

  const double clearedSide = clearedModules * layout.moduleSize;
  layout.glyphSide = clearedSide - 2 * layout.outlineThickness;
  if (layout.glyphSide < 0) {
    // an absurdly small render: draw the glyph, drop the outline rather than
    // invert it
    layout.glyphSide = clearedSide;
    layout.outlineThickness = 0;
  }
  layout.glyphLeft = (layout.side - layout.glyphSide) / 2;
  layout.glyphTop = layout.glyphLeft;
  return layout;
}

std::vector<ExtenderQrRun> ExtenderQrRunsFor(int moduleCount, const std::vector<bool>& dark,
                                             int clearFrom, int clearTo) {
  std::vector<ExtenderQrRun> runs;
  if (moduleCount <= 0) return runs;
  const std::size_t need = static_cast<std::size_t>(moduleCount) *
                           static_cast<std::size_t>(moduleCount);
  if (dark.size() < need) return runs;
  for (int y = 0; y < moduleCount; ++y) {
    const bool clearedRow = clearFrom <= y && y < clearTo;
    int start = -1;
    for (int x = 0; x <= moduleCount; ++x) {
      const bool cleared = clearedRow && clearFrom <= x && x < clearTo;
      const bool on = x < moduleCount && !cleared &&
                      dark[static_cast<std::size_t>(y) * static_cast<std::size_t>(moduleCount) +
                           static_cast<std::size_t>(x)];
      if (on && start < 0) {
        start = x;
      } else if (!on && 0 <= start) {
        runs.push_back(ExtenderQrRun{start, y, x - start});
        start = -1;
      }
    }
  }
  return runs;
}

// ---- import -----------------------------------------------------------------

ExtenderImportDecision DecideExtenderImport(const ExtenderShareDecodeView& decoded,
                                            bool useSettings) {
  ExtenderImportDecision decision;
  if (!decoded.ok) {
    // The SDK already named the reason, as a key id. An empty one still has to
    // say something, and "this is not an extender share" is the only thing a
    // decode that failed without a reason can honestly claim.
    decision.messageKey =
        decoded.error.empty() ? std::string("import_extenders_invalid") : decoded.error;
    if (decision.messageKey == "import_extenders_foreign_host") {
      decision.showForeignHost = true;
      decision.messageArg =
          decoded.networkHost.empty() ? decoded.settingsHost : decoded.networkHost;
    }
    return decision;
  }

  decision.showSettingsToggle = decoded.hasSettings;
  decision.showForeignHost = decoded.foreignHost;
  if (decoded.foreignHost) {
    decision.messageKey = "import_extenders_foreign_host";
    decision.messageArg = decoded.networkHost;
  }

  const bool applyingSettings = useSettings && decoded.hasSettings;
  // A foreign payload is importable only WITH its settings: without them the
  // addresses would be judged against an operator that never signed them.
  decision.canImport = !decoded.foreignHost || applyingSettings;
  decision.needsConfirm = applyingSettings;
  if (applyingSettings) {
    decision.confirmArg =
        decoded.settingsHost.empty() ? decoded.networkHost : decoded.settingsHost;
  }
  // Named whether or not the settings are taken yet: the line is what the user
  // weighs before turning them on.
  if (decoded.hasSettings) {
    for (const std::string& url : decoded.controlDohUrls) {
      if (!decision.controlDohUrlsArg.empty()) decision.controlDohUrlsArg += ", ";
      decision.controlDohUrlsArg += url;
    }
  }
  return decision;
}

ExtenderImportOutcome ExtenderImportOutcomeFor(const ExtenderImportResultView& result) {
  ExtenderImportOutcome outcome;
  if (!result.ok) {
    outcome.messageKey =
        result.error.empty() ? std::string("import_extenders_invalid") : result.error;
    return outcome;
  }
  outcome.ok = true;
  outcome.isPlural = true;
  outcome.messageKey = "import_extenders_imported";
  outcome.count = 0 < result.importedCount ? result.importedCount : 0;
  return outcome;
}

// ---- the provider extender row ------------------------------------------------

namespace {

// The families of an active row: both, else the one that holds. The SDK
// reports active only with a family activated; a device that ever said active
// with neither would read as IPv4, the first of the two.
const char* ExtenderProvideFamiliesKey(bool activatedV4, bool activatedV6) {
  if (activatedV4 && activatedV6) return "ipv4_and_ipv6";
  if (activatedV6) return "ipv6";
  return "ipv4";
}

// The label of an error case that carries a Reason; null for a case this build
// does not know.
const char* ExtenderProvideErrorKey(std::string_view errorCase) {
  if (errorCase == kExtenderProvideErrorStart) return "extender_start_failed";
  if (errorCase == kExtenderProvideErrorTcpUnavailable) return "extender_tcp_unavailable";
  if (errorCase == kExtenderProvideErrorListen) return "extender_listen_failed";
  if (errorCase == kExtenderProvideErrorActivationRefused) return "extender_activation_refused";
  if (errorCase == kExtenderProvideErrorActivationUnreachable) return "extender_activation_unreachable";
  if (errorCase == kExtenderProvideErrorActivationFailed) return "extender_activation_failed";
  return nullptr;
}

}  // namespace

ExtenderProvideRowModel ExtenderProvideRowModelFor(const ExtenderProvideStatusView& view) {
  ExtenderProvideRowModel model;
  model.visible = view.supported;
  // the switch writes through a session's device, or for the provider-only
  // device's status through the service, when it takes the write
  model.switchVisible = view.supported && (!view.providerOnly || view.serviceWritable);
  model.on = view.provideExtender;
  const std::string_view state = view.state;
  if (state == kExtenderProvideStateOff) {
    model.textKey = "off";
  } else if (state == kExtenderProvideStateNotProviding) {
    model.textKey = "extender_not_providing";
  } else if (state == kExtenderProvideStateSettingUp) {
    model.tone = ExtenderProvideTone::Yellow;
    model.textKey = "extender_setting_up";
  } else if (state == kExtenderProvideStateActive) {
    model.tone = ExtenderProvideTone::Green;
    model.textKey = "extender_active";
    model.argument = ExtenderProvideFamiliesKey(view.activatedV4, view.activatedV6);
    model.argumentKind = ExtenderProvideArgument::Key;
    // ErrorCase is empty in this state: the Reason alone is the other family's
    // text, and LastActivationRefused says which of the two labels it takes
    if (!view.reason.empty()) {
      model.suffixKey =
          view.refused ? "extender_activation_refused" : "extender_activation_failed";
      model.suffixArgument = view.reason;
    }
  } else if (state == kExtenderProvideStateError) {
    model.tone = ExtenderProvideTone::Red;
    if (view.errorCase == kExtenderProvideErrorRevoked) {
      // the case is the whole message; the SDK sends no reason with it
      model.textKey = "extender_revoked";
    } else {
      const char* key = ExtenderProvideErrorKey(view.errorCase);
      // a case this build does not know renders the Reason bare, still red
      model.textKey = key ? key : "";
      model.argument = view.reason;
      model.argumentKind = ExtenderProvideArgument::Text;
    }
  } else {
    model.textKey.clear();
    model.argument = view.reason;
    model.argumentKind = ExtenderProvideArgument::Text;
  }
  return model;
}

ExtenderProvideStatusView ExtenderProvideGuessFor(const ExtenderProvideStatusView& current,
                                                  bool on, bool providing) {
  ExtenderProvideStatusView guess;
  guess.supported = current.supported;
  guess.providerOnly = current.providerOnly;
  // the switch that was just flipped stays shown over its own guess
  guess.serviceWritable = current.serviceWritable;
  guess.state = !on        ? kExtenderProvideStateOff
                : providing ? kExtenderProvideStateSettingUp
                            : kExtenderProvideStateNotProviding;
  // the role runs exactly when the guess says setting up
  guess.enabled = on && providing;
  guess.provideExtender = on;
  // the speed tests switch under it keeps its place and its position
  guess.measurementsEnabled = current.measurementsEnabled;
  guess.measurementsServiceWritable = current.measurementsServiceWritable;
  // and so does the router port mapping switch under that
  guess.provideExtenderPortMapping = current.provideExtenderPortMapping;
  guess.portMappingServiceWritable = current.portMappingServiceWritable;
  return guess;
}

ExtenderProvideWriteRoute ExtenderProvideWriteRouteFor(bool sessionDevice,
                                                       const ExtenderProvideStatusView& shown) {
  if (sessionDevice) return ExtenderProvideWriteRoute::Device;
  // exactly the status the switch shows over with no session
  if (shown.supported && shown.providerOnly && shown.serviceWritable) {
    return ExtenderProvideWriteRoute::Service;
  }
  return ExtenderProvideWriteRoute::None;
}

// ---- the lag.dev speed tests switch -------------------------------------------

SpeedTestsRowModel SpeedTestsRowModelFor(const ExtenderProvideStatusView& view) {
  SpeedTestsRowModel model;
  // beside the Extender switch, and for the provider-only device's status only
  // when the service takes this setting's write as well
  model.visible = ExtenderProvideRowModelFor(view).switchVisible &&
                  (!view.providerOnly || view.measurementsServiceWritable);
  model.on = view.measurementsEnabled;
  return model;
}

ExtenderProvideWriteRoute SpeedTestsWriteRouteFor(bool sessionDevice,
                                                  const ExtenderProvideStatusView& shown) {
  if (sessionDevice) return ExtenderProvideWriteRoute::Device;
  // exactly the status the switch shows over with no session
  if (shown.supported && shown.providerOnly && shown.serviceWritable &&
      shown.measurementsServiceWritable) {
    return ExtenderProvideWriteRoute::Service;
  }
  return ExtenderProvideWriteRoute::None;
}

ExtenderProvideStatusView SpeedTestsGuessFor(const ExtenderProvideStatusView& current, bool on) {
  ExtenderProvideStatusView guess = current;
  guess.measurementsEnabled = on;
  return guess;
}

// ---- the router port mapping switch -------------------------------------------

PortMappingRowModel PortMappingRowModelFor(const ExtenderProvideStatusView& view) {
  PortMappingRowModel model;
  // under the speed tests switch, never without it, and for the provider-only
  // device's status only when the service takes this setting's write as well
  model.visible = SpeedTestsRowModelFor(view).visible &&
                  (!view.providerOnly || view.portMappingServiceWritable);
  model.on = view.provideExtenderPortMapping;
  return model;
}

ExtenderProvideWriteRoute PortMappingWriteRouteFor(bool sessionDevice,
                                                   const ExtenderProvideStatusView& shown) {
  if (sessionDevice) return ExtenderProvideWriteRoute::Device;
  // exactly the status the switch shows over with no session: the one the
  // speed tests switch's write goes to the service for, when the service takes
  // this write too
  if (SpeedTestsWriteRouteFor(false, shown) == ExtenderProvideWriteRoute::Service &&
      shown.portMappingServiceWritable) {
    return ExtenderProvideWriteRoute::Service;
  }
  return ExtenderProvideWriteRoute::None;
}

ExtenderProvideStatusView PortMappingGuessFor(const ExtenderProvideStatusView& current, bool on) {
  ExtenderProvideStatusView guess = current;
  guess.provideExtenderPortMapping = on;
  return guess;
}

// ---- the statistics sections ----------------------------------------------------

ExtenderStatsSections ExtenderStatsSectionsFor(bool providingEnabled, bool hasProviderStats,
                                               bool extenderRunning) {
  ExtenderStatsSections sections;
  sections.providerVisible = providingEnabled && hasProviderStats;
  sections.extenderVisible = sections.providerVisible && extenderRunning;
  sections.disabledMeta = !sections.providerVisible;
  return sections;
}

}  // namespace urnw
