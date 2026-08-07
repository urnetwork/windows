// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Foundation.h>

#include "AppController.h"
#include "Log.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace winrt::URnetwork::implementation {
namespace {

// Where one pane sits: which cell of its page's pane grid, how many rows it may
// span, and what gap it keeps. Every destination's wide reading is two of these
// - one for the flyout, one for the desktop - and nothing else.
struct PanePlacement {
  int row = 0;
  int column = 0;
  int rowSpan = 1;
  Thickness margin{};
};

void Place(FrameworkElement const& pane, PanePlacement const& at) {
  if (!pane) return;
  Grid::SetRow(pane, at.row);
  Grid::SetColumn(pane, at.column);
  Grid::SetRowSpan(pane, at.rowSpan);
  pane.Margin(at.margin);
}

// A column's width, as a fixed number of DIPs. Zero collapses it, which is how
// a side column stops existing at flyout widths.
void SetWidth(Controls::ColumnDefinition const& column, double dips) {
  if (column) column.Width(GridLengthHelper::FromPixels(dips));
}

// A column's width as a share of what is left, for the two destinations whose
// split is a PROPORTION rather than a rail: a table and the panels beside it
// should both grow when the window does, and a fixed side column would leave
// all of the extra to one of them.
void SetStar(Controls::ColumnDefinition const& column, double weight) {
  if (column) column.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
}

}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();
  // The window chrome: content extends under the system caption buttons, and
  // AppTitleBar is the DRAG region. Naming a drag region is what separates an
  // actual title bar from a wordmark drawn where one would be. The rest of the
  // native shell (backdrop, caption-button colours, size, placement) needs the
  // HWND and is applied by urnw::shell::ApplyNativeShell from AppController.
  ExtendsContentIntoTitleBar(true);
  SetTitleBar(AppTitleBar());

  // Each destination owns its own translation unit; the window keeps navigation,
  // the auth + balance relays, and the shared sheet guard. Constructed before
  // ApplyStrings because every page paints its own labels from there.
  login_ = std::make_unique<urnw::LoginPage>(*this);
  connect_ = std::make_unique<urnw::ConnectPage>(*this);
  account_ = std::make_unique<urnw::AccountPage>(*this);
  wallet_ = std::make_unique<urnw::WalletPage>(*this);
  settings_ = std::make_unique<urnw::SettingsPage>(*this);
  // Last: its ctor binds SdkHost's mode-notice handler and asks for a refresh,
  // so everything it may paint over must already exist.
  developer_ = std::make_unique<urnw::DeveloperPage>(*this);

  // The responsive switch. SizeChanged on the window's own content root fires
  // on the first layout pass, so this also seeds the initial state; the handler
  // is cheap on the resizes that do not cross the breakpoint.
  if (auto root = Content().try_as<FrameworkElement>()) {
    root.SizeChanged([weak = get_weak()](auto const&, auto const&) {
      if (auto self = weak.get()) self->ApplyBreakpoint();
    });
  }

  ApplyStrings();
  // Before connect_->Initialize(): ConnectPage::ApplyConnectStatus pushes the
  // connection half of the strip, and it must have somewhere to push it to.
  BuildStatusStrip();
  connect_->Initialize();  // charts, SDK feeds, card affordances, drawer clock

  // plan + usage cards (account panel and connect drawer)
  accountUsageBar_ = std::make_unique<urnw::UsageBar>(AccountUsageBarHost(),
                                                      AccountUsageLegend());
  drawerUsageBar_ = std::make_unique<urnw::UsageBar>(DrawerUsageBarHost(),
                                                     DrawerUsageLegend());

  // the insufficient-balance warning's action opens the upgrade flow (a guest
  // first creates a full account, like the plan card's affordance)
  {
    Button getPro;
    getPro.Content(LocBox("become_supporter"));
    getPro.Click([weak = get_weak()](auto const&, auto const&) {
      auto self = weak.get();
      if (!self) return;
      if (self->balance_.guest) {
        self->login().BeginGuestUpgrade();
      } else {
        self->ShowUpgradeSheet();
      }
    });
    BalanceWarning().ActionButton(getPro);
  }

  login_->Initialize();   // name / bonus-code debounce + resend cooldown
  wallet_->Initialize();  // wallet-address validation debounce

  // seed the plan cards from the balance store's current snapshot
  balance_ = Balance().Current();
  balancePoll_ = Balance().CurrentPoll();
  ApplyBalance();

  ApplyAuthState(Sdk().IsLoggedIn() ? urnw::AuthState::LoggedIn
                                    : urnw::AuthState::LoggedOut,
                 "");
  if (Sdk().IsLoggedIn() && Sdk().apiReady()) account_->LoadReferralInfo();
  connect_->ResyncDrawer();

  // THE MODE NOTICE CHANNEL HAD NO CONSUMER. SdkHost::SetModeNoticeHandler was
  // never called anywhere in the app, so onModeNotice_ was permanently null and
  // every notice ever published — the rpc-only banner AND every "there is no
  // session, and here is why" — went into the void. It was found from the other
  // end: after RegisterNetworkClient stopped reporting a tunnel-bootstrap
  // failure as an AUTHENTICATION failure, the replacement signal turned out not
  // to exist, and a seedphrase sign-in with no service running landed silently
  // on a home screen that could never connect.
  //
  // The window-level snackbar is what this app has. It is transient where a
  // standing banner would be better, and that is worth doing properly — but a
  // transient signal beats the nothing that was there.
  //
  // Marshalled: SdkHost invokes this with mutex_ HELD on the RefreshModeNotice
  // path, so touching XAML or calling back into SdkHost inline would deadlock.
  {
    auto queue = DispatcherQueue();
    Sdk().SetModeNoticeHandler([weak = get_weak(), queue](
                                   urnw::SdkHost::ModeNotice const& notice) {
      const bool active = notice.active;
      const bool failed = notice.kind == urnw::SdkHost::ModeNotice::Kind::SessionFailed;
      auto message = winrt::to_hstring(notice.message);
      queue.TryEnqueue([weak, active, failed, message] {
        auto self = weak.get();
        if (!self || !active || message.empty()) return;
        self->login().ShowModeNotice(message, failed);
      });
    });
    // Anything that failed BEFORE this handler existed is standing state in
    // SdkHost (sessionFailure_); this is what replays it.
    Sdk().RefreshModeNotice();
  }
}

MainWindow::~MainWindow() {
  // the page dtors stop their own timers; order them explicitly rather than
  // relying on member declaration order
  connect_.reset();
  wallet_.reset();
  login_.reset();
  account_.reset();
  settings_.reset();
  developer_.reset();
}

void MainWindow::SetPresentationActive(bool active) {
  connect_->SetPresentationActive(active);
  developer_->SetPresentationActive(active);
  // the login carousel's timer: it runs only while the window is on screen
  login_->SetPresentationActive(active);
}

// ---- strings -------------------------------------------------------------
// Every label in the window, from the shared localization store (Localization.h).
// The XAML carries no user-facing text: the ids below are the store's key ids, so
// a missing key renders as the id itself instead of an empty control. Runs once,
// on the UI thread, before the window is shown; the dynamic labels (status,
// counts, verdicts) are seeded here and then owned by the state relays.

void MainWindow::ApplyStrings() {
  Title(Loc("app_name"));
  BrandText().Text(Loc("app_name"));

  // navigation
  ConnectNavItem().Content(LocBox("connect"));
  AccountNavItem().Content(LocBox("account"));
  WalletNavItem().Content(LocBox("wallet"));
  LeaderboardNavItem().Content(LocBox("leaderboard"));
  SupportNavItem().Content(LocBox("support"));
  SettingsNavItem().Content(LocBox("settings"));
  // DeveloperNavItem's label is painted by the page: the store has no key for
  // the developer surface yet, so it goes through the same fallback the rest of
  // that screen uses (DeveloperPage.cpp, Dev()).

  login_->ApplyStrings();
  connect_->ApplyStrings();
  account_->ApplyStrings();
  wallet_->ApplyStrings();
  settings_->ApplyStrings();
  developer_->ApplyStrings();
}

// ---- the one responsive switch (D4) ----------------------------------------
//
// Every destination declares the SAME two states in markup - Narrow (what the
// markup plainly says) and Wide (the horizontal composition) - and this is the
// only thing in the app that switches between them. One handler, one number,
// seven groups: a per-page SizeChanged would be seven places for the app to
// stop agreeing with itself about what "wide" means.
//
// It is code, and VisualStateManager was tried first and does not work in this
// shell. Both halves were measured, not read in a doc:
//
//   * AdaptiveTrigger never fires. It listens for size changes on
//     Window.Current, which is null in a WinUI 3 desktop app. A trigger with
//     MinWindowWidth="1" and one Margin setter changed nothing at 1400px.
//   * With a plain boolean StateTrigger flipped from here, the trigger DID go
//     active (the log line below proved it) and the Setters still did not
//     apply: VisualStateGroups attached to a plain layout Grid are never
//     processed, because nothing bootstraps them. GoToState is no help - it
//     takes a Control and reads the groups off that control's TEMPLATE root -
//     and WinUI 3 has no GoToElementState. Wrapping each destination in a
//     templated ContentControl would work and would also move every x:Name in
//     this file into a template namescope, i.e. delete every accessor the
//     seven page units are written against.
//
// So the markup keeps the SHAPE (named columns, named panes, the narrow
// reading as the plain reading) and this one function carries the differences.
// One function rather than seven page-level SizeChanged handlers is the point:
// there is exactly one place where the app decides what "wide" means.

void MainWindow::ApplyBreakpoint() {
  auto root = Content().try_as<FrameworkElement>();
  if (!root) return;
  // ActualWidth is in DIPs, which is what the breakpoint is stated in: on this
  // machine's 125% display a "1400px" window is 1120dip, and a breakpoint
  // compared against physical pixels would fire in the wrong place on every
  // machine with a different scale.
  const double width = root.ActualWidth();
  if (width <= 0) return;
  const bool wide = urnw::kit::kWideBreakpointDip <= width;
  if (breakpointApplied_ && wide == wideLayout_) return;
  breakpointApplied_ = true;
  wideLayout_ = wide;

  // ---- Connect: Proton's shape ---------------------------------------------
  // The hero keeps the main canvas; Provide, Connect options and the plan card
  // become a rail BESIDE it rather than a stack under it. 1440 rather than "as
  // wide as the window": a 2560px hero is not composed, it is stretched.
  ConnectCapColumn().MaxWidth(wide ? 1440 : 600);
  SetWidth(ConnectRailColumn(), wide ? 360 : 0);
  Place(ConnectRail(), wide
            // Beside the hero, in the hero's OWN row and spanning nothing.
            // RowSpan=3 was tried and is wrong: WinUI spreads a spanning
            // element's desired height across every Auto row it covers, so a
            // rail taller than the hero inflated the two rows under it and
            // opened ~120px of blank between the hero card and the charts.
            // In one row the columns simply run ragged, which is what a
            // two-column page does.
            ? PanePlacement{0, 1, 1, Thickness{20, 0, 0, 24}}
            // directly under the hero card, which is where the flyout has
            // always had it
            : PanePlacement{1, 0, 1, Thickness{0, 16, 0, 0}});

  // ---- Wallet: Portmaster's master-detail ----------------------------------
  // The figures across the top; the sources of the money on the left (wallets,
  // points, multipliers, reliability); the ledger on the right. The split is a
  // proportion rather than a rail because BOTH sides want the extra: a payouts
  // table with four columns and a row of wallet cards both read better wider.
  WalletCapColumn().MaxWidth(wide ? 1720 : 820);
  if (wide) {
    SetStar(WalletSideColumn(), 0.85);
  } else {
    SetWidth(WalletSideColumn(), 0);
  }
  Place(WalletSideStack(), wide ? PanePlacement{1, 1, 1, Thickness{20, 4, 0, 24}}
                                : PanePlacement{2, 0, 1, Thickness{0, 4, 0, 24}});
  // The three header figures: a row at desktop widths, a column at flyout
  // width. Three tiles across 560dip read "Unpaid data provid..." and
  // "1234.50..." - a KPI that cannot be read is not a KPI.
  SetStar(WalletStatsColumn2(), wide ? 1 : 0);
  SetStar(WalletStatsColumn3(), wide ? 1 : 0);
  Place(WalletPendingTile(),
        wide ? PanePlacement{0, 1, 1, Thickness{12, 0, 0, 0}}
             : PanePlacement{1, 0, 1, Thickness{0, 8, 0, 0}});
  Place(WalletReferralsTile(),
        wide ? PanePlacement{0, 2, 1, Thickness{12, 0, 0, 0}}
             : PanePlacement{2, 0, 1, Thickness{0, 8, 0, 0}});

  // ---- Account: the plan beside the identity -------------------------------
  // The usage bar is the widest thing on this destination and the only one that
  // reads better for it, so the plan card keeps the main column; redeemed
  // codes, the profile form and referrals go beside it instead of a screenful
  // below it. 1180 rather than Wallet's 1720: there is no table here, and two
  // ~580dip card columns is as wide as a column of form rows should ever get.
  AccountCapColumn().MaxWidth(wide ? 1180 : 560);
  if (wide) {
    SetStar(AccountSideColumn(), 1);
  } else {
    SetWidth(AccountSideColumn(), 0);
  }
  Place(AccountSideStack(), wide ? PanePlacement{0, 1, 1, Thickness{20, 0, 0, 24}}
                                 : PanePlacement{1, 0, 1, Thickness{0, 0, 0, 24}});

  // ---- Leaderboard: the table, with your own rank beside it ----------------
  // The plainest cut in the app. BOTH panes move here, because the reading
  // order inverts: at flyout width the one thing you came for is your own
  // number, so the rank card is first; at desktop widths the table is the page
  // and the rank card is the note beside it.
  // 1360, not Wallet's 1720: these rows carry three fields, and past about a
  // thousand dips the gap between a network's name and its figure stops being
  // a table and starts being two lists.
  LeaderboardCapColumn().MaxWidth(wide ? 1360 : 820);
  if (wide) {
    SetWidth(LeaderboardSideColumn(), 360);
  } else {
    SetWidth(LeaderboardSideColumn(), 0);
  }
  Place(LeaderboardMainStack(), wide ? PanePlacement{0, 0, 1, Thickness{0, 0, 0, 24}}
                                     : PanePlacement{1, 0, 1, Thickness{0, 12, 0, 24}});
  Place(LeaderboardSideStack(), wide ? PanePlacement{0, 1, 1, Thickness{20, 0, 0, 24}}
                                     : PanePlacement{0, 0, 1, Thickness{}});

  // ---- Settings: two card columns ------------------------------------------
  // A wall of cards and nothing else, so the wide reading is simply two
  // columns of them. Same 1180 cap as Account, and for the same reason: a
  // settings row is a label and a control, and past ~580dip they stop looking
  // like they belong to each other. The destructive end stays full width under
  // both columns - see the markup.
  SettingsCapColumn().MaxWidth(wide ? 1180 : 560);
  if (wide) {
    SetStar(SettingsSideColumn(), 1);
  } else {
    SetWidth(SettingsSideColumn(), 0);
  }
  Place(SettingsSideStack(), wide ? PanePlacement{0, 1, 1, Thickness{20, 0, 0, 0}}
                                  : PanePlacement{1, 0, 1, Thickness{0, 16, 0, 0}});

  urnw::LogInfo("layout: {} at {:.0f}dip", wide ? "wide" : "narrow", width);
}

// ---- the persistent status strip (D4) --------------------------------------
//
// ProtonVPN pins an IP / country / provider line to the bottom of the window on
// every destination. This app had no equivalent: the connection state existed
// only on Connect, so on five of the seven destinations there was no way at all
// to tell whether traffic was flowing without navigating away from what you
// were doing.
//
// Three facts, in the order Proton reads them: WHAT the client is doing (a
// state-coloured dot and the same wording the connect screen uses), WHOSE
// network it is doing it on, and THROUGH WHOM. The traffic field closes it,
// because "connected" and "carrying packets" are different claims and this
// client can be the first without being the second (an rpc-only session is
// exactly that, and LiveStats clamps its counters to say so).
//
// Built rather than declared: the product tiering decision (spec 511c26c) makes
// this the flagship Advanced Mode surface, and four more fields must cost four
// more calls to MakeStatusField.

void MainWindow::BuildStatusStrip() {
  auto fields = StatusStripFields();
  fields.Children().Clear();
  statusSessionParts_.clear();

  // The strip as a whole is a landmark: one accessible name over the row, so a
  // screen reader announces "URnetwork Status" and then the fields, instead of
  // four unrelated values at the end of every page.
  Automation::AutomationProperties::SetName(StatusStrip(), Loc("urnetwork_status"));

  // Field 1 carries NO caption. The dot and the word beside it already say what
  // they are, and "URnetwork Status: Connected" in a 12sp strip is the caption
  // spending more room than the fact. Its accessible name supplies what the
  // caption would have.
  statusState_ = urnw::kit::MakeStatusField(hstring{}, /*withDot=*/true,
                                            Loc("urnetwork_status"));
  fields.Children().Append(statusState_.root);

  auto section = [&](urnw::kit::StatusField& field, std::string_view labelKey) {
    auto rule = urnw::kit::MakeStatusSeparator();
    fields.Children().Append(rule);
    field = urnw::kit::MakeStatusField(Loc(labelKey));
    fields.Children().Append(field.root);
    statusSessionParts_.push_back(rule);
    statusSessionParts_.push_back(field.root);
  };
  section(statusNetwork_, "network");
  section(statusProvider_, "selected_provider");
  section(statusTraffic_, "data");

  ApplyStatusStrip();
}

void MainWindow::RenderStatusState(hstring const& text,
                                   winrt::Windows::UI::Color dot) {
  if (!statusState_.value) return;
  statusState_.value.Text(text);
  // The state field is the one line on the strip that is never muted: it is the
  // fact the other three qualify.
  statusState_.value.Foreground(urnw::colors::TextBrush());
  if (statusState_.dot) statusState_.dot.Fill(urnw::colors::MakeBrush(dot));
}

void MainWindow::ApplyStatusStripConnection(hstring const& text,
                                            winrt::Windows::UI::Color dot) {
  if (statusSamplePinned_) return;
  // With no session the connect page still has a status line, and its idle
  // wording is "Ready to connect" - which on the strip would be a standing
  // claim that the client is one press away from carrying traffic when it has
  // no account at all. Measured on a signed-out preview run: the strip read
  // "Ready to connect" under a Wallet page with nothing on it. The signed-out
  // reading belongs to ApplyStatusStrip and wins here.
  if (!statusSignedIn_) return;
  RenderStatusState(text, dot);
}

void MainWindow::ApplyStatusStrip() {
  if (!statusNetwork_.value) return;  // not built yet

  // The strip belongs to the signed-in shell. On the sign-in flow it would be a
  // status line about a session that does not exist, under a screen whose whole
  // subject is creating one.
  const bool home = HomeNav().Visibility() == Visibility::Visible;
  StatusStrip().Visibility(home ? Visibility::Visible : Visibility::Collapsed);
  if (!home) return;

  // No session (a signed-out shell, which is what --preview-ui shows): say so
  // once and hide the three fields that would be captions over blanks. The
  // store has no "Not signed in" string, so this uses the instruction it does
  // have; see the D4 report.
  if (!statusSignedIn_) {
    RenderStatusState(Loc("sign_in"), urnw::colors::kTextFaint);
    for (auto const& part : statusSessionParts_) part.Visibility(Visibility::Collapsed);
    return;
  }
  for (auto const& part : statusSessionParts_) part.Visibility(Visibility::Visible);

  statusNetwork_.value.Text(statusGuest_ || statusNetworkName_.empty()
                                ? Loc("guest")
                                : H(statusNetworkName_));
  statusProvider_.value.Text(statusLocationName_.empty()
                                 ? Loc("best_available_provider")
                                 : H(statusLocationName_));
  // "Connected" and "carrying traffic" are different claims. An rpc-only
  // session reports the first and none of the second, and LiveStats has already
  // clamped its counters to zero for exactly that reason - so this reads the
  // clamped values and says what they mean rather than printing two zeroes.
  statusTraffic_.value.Text(
      statusConnected_ ? H("↓ " + urnw::FormatBitRate(statusDownBps_) + "   ↑ " +
                           urnw::FormatBitRate(statusUpBps_))
                       : Loc("site_app_no_traffic"));
}

// Two gates, as WalletPage's sample rows have: --preview-ui says there is no
// session, and URNETWORK_PREVIEW_SAMPLE says the operator asked for synthetic
// content. Without this the strip could only ever be screenshotted in its
// signed-out state, which is the one state that does not exercise its layout.
void MainWindow::PreviewSampleStatusStrip() {
  if (!previewUi_) return;
  size_t len = 0;
  char value[16]{};
  if (getenv_s(&len, value, sizeof(value), "URNETWORK_PREVIEW_SAMPLE") != 0 || len == 0) {
    return;
  }
  if (std::string_view(value) != "1") return;
  urnw::LogWarn(
      "preview-sample: rendering a SYNTHETIC status strip - no session, no api, "
      "none of these values came from the network");
  statusSignedIn_ = true;
  statusNetworkName_ = "sample-network";
  statusGuest_ = false;
  statusConnected_ = true;
  statusLocationName_ = "Frankfurt, Germany";
  statusDownBps_ = 24'600'000;
  statusUpBps_ = 3'100'000;
  ApplyStatusStrip();
  RenderStatusState(Loc("connected"), urnw::colors::kUrGreen);
  // Pinned LAST: the two calls above are the ones that must land, and every
  // real push after this point is a push from a session that does not exist.
  statusSamplePinned_ = true;
}

// ---- login / home roots ----------------------------------------------------

void MainWindow::ShowLoginRoot() {
  HomeNav().Visibility(Visibility::Collapsed);
  LoginRoot().Visibility(Visibility::Visible);
  ApplyStatusStrip();
}

void MainWindow::ShowHomeRoot() {
  LoginRoot().Visibility(Visibility::Collapsed);
  HomeNav().Visibility(Visibility::Visible);
  ApplyStatusStrip();
}

// --preview-ui. This does not authenticate: no token is read or written and no
// session is created. It forces the root swap, picks a destination, and lets the
// panels render against the empty local snapshots — which is the state a real
// signed-in user's FIRST frame is in, before any response lands.
//
// It is NOT a promise of "no network". Selecting a destination runs the same
// navigation relay a real user does, and that relay is what would issue the
// per-destination API loads; those are skipped explicitly in
// OnNavSelectionChanged, because there is no token and the only thing they
// could produce is a 401. The drawer's own EnsureLocations still runs, exactly
// as it does on a normal signed-out launch.
void MainWindow::EnterPreviewUi(std::string const& destination) {
  previewUi_ = true;
  urnw::LogInfo("preview-ui: showing the signed-in shell at '{}' with no session",
                destination);
  ShowHomeRoot();
  // The strip's signed-out reading is the one a preview run would otherwise be
  // stuck in, and it is the one that does not exercise its layout. This is
  // gated a second time, on URNETWORK_PREVIEW_SAMPLE.
  PreviewSampleStatusStrip();

  // "seedphrase" is a modal, not a destination: show the connect drawer behind
  // it and raise the sheet. It is the only surface in the app that cannot be
  // reached any other way — it appears once, right after an account is
  // created, and creating one on a dev box is exactly what must not happen.
  if (destination == "seedphrase") {
    login_->ShowPreviewSeedphraseSheet();
    return;
  }

  const hstring tag = H(destination);
  for (auto const& item : {ConnectNavItem(), AccountNavItem(), WalletNavItem(),
                           LeaderboardNavItem(), SupportNavItem(), SettingsNavItem(),
                           DeveloperNavItem()}) {
    if (winrt::unbox_value_or<hstring>(item.Tag(), L"") == tag) {
      HomeNav().SelectedItem(item);  // the SelectionChanged relay shows the panel
      break;
    }
  }

  connect_->ResyncDrawer();
  if (ConnectView().Visibility() == Visibility::Visible) connect_->AnimateDrawerIn();

  // Both snackbar call sites are signed-in-only and had never rendered. Raise
  // the one belonging to the destination being previewed, and deliberately pick
  // opposite severities across the two so both behaviours are visible: the
  // wallet error must PERSIST, the support acknowledgement must time out.
  if (destination == "wallet") wallet_->ShowPreviewSnackbar();
  if (destination == "support") settings_->ShowPreviewSnackbar();
  // The per-destination preview states are NOT raised here any more. They
  // belong to whichever destination is selected, at the moment it is selected,
  // and this function runs exactly once — so gating them on the LAUNCH tag left
  // every other destination stranded: `--preview-ui=leaderboard`, then click
  // Wallet, and Payout Wallets / Account points / Network reliability / Payouts
  // all sat on "Loading..." for good, which is precisely the hang this switch
  // exists to rule out (screenshotted). OnNavSelectionChanged settles them on
  // every navigation instead, and the SelectedItem assignment above has already
  // been through it.
}

// ---- navigation ----------------------------------------------------------

void MainWindow::OnNavSelectionChanged(NavigationView const&,
                                       NavigationViewSelectionChangedEventArgs const& args) {
  auto item = args.SelectedItem().try_as<NavigationViewItem>();
  if (!item) return;
  const auto tag = winrt::unbox_value_or<hstring>(item.Tag(), L"connect");
  // LeftMinimal collapses the destination list behind the pane toggle, so the
  // header is the only thing telling the user where they are. The item's own
  // Content is already the localized name (ApplyStrings), so this needs no new
  // string and cannot drift from the menu.
  HomeNav().Header(item.Content());

  const bool wasConnectVisible = ConnectView().Visibility() == Visibility::Visible;
  ConnectView().Visibility(tag == L"connect" ? Visibility::Visible : Visibility::Collapsed);
  AccountView().Visibility(tag == L"account" ? Visibility::Visible : Visibility::Collapsed);
  WalletView().Visibility(tag == L"wallet" ? Visibility::Visible : Visibility::Collapsed);
  LeaderboardView().Visibility(tag == L"leaderboard" ? Visibility::Visible : Visibility::Collapsed);
  SupportView().Visibility(tag == L"support" ? Visibility::Visible : Visibility::Collapsed);
  SettingsView().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);
  DeveloperView().Visibility(tag == L"developer" ? Visibility::Visible
                                                 : Visibility::Collapsed);
  // The developer screen's 5s poll is four synchronous rpcs into the service:
  // it runs only while this destination is selected AND the window is
  // presenting (SetPresentationActive supplies the other half).
  developer_->SetSelected(tag == L"developer");

  if (tag == L"connect" && !wasConnectVisible) connect_->AnimateDrawerIn();

  // --preview-ui has no session. apiReady() is NOT the guard for that: it is
  // api_.has_value(), set at SDK INIT, not at login — so without this the
  // preview fired LoadAccount, Balance().Refresh(), LoadWallet() and
  // LoadLeaderboard() with no token, and the API answered 401 four times. A
  // development switch that ships in Release must not talk to production.
  if (previewUi_) {
    urnw::LogInfo("preview-ui: '{}' selected - skipping its API loads (no session)",
                  urnw::Narrow(std::wstring{tag}));
    // Skipping the loads is only half of it: a panel whose fetch never runs sits
    // on "Loading..." for good, which is indistinguishable from a hang. Settle
    // the destination the user just navigated TO, every time - not only the one
    // named on the command line, which is what EnterPreviewUi used to do and
    // which left Wallet stranded for anyone who arrived from another tag.
    if (tag == L"wallet") wallet_->ShowPreviewWalletState();
    if (tag == L"leaderboard") wallet_->ShowPreviewLeaderboardState();
    // Carried over from EnterPreviewUi at merge. P4 moved the per-destination
    // preview states here and P2 added the developer surface in parallel, so
    // taking either side wholesale would have dropped the other's: P4's branch
    // predates DeveloperPage entirely. The notice needs a session to fire and
    // every card below the intro is gated on a live settings read, so without
    // these two a preview run reaches the destination and finds it blank.
    if (tag == L"developer") {
      developer_->ShowPreviewModeNotice();
      developer_->ShowPreviewSnapshot();
    }
    return;
  }
  LoadCurrentDestination();
}

// The per-destination API loads, keyed off whatever destination is selected NOW.
//
// This is called from the navigation relay AND from the auth relay, and the
// second caller is the load-bearing one. Selecting a destination was the only
// thing that ever ran these, and the selection SURVIVES a sign-out: the sign-out
// button is itself on the Settings page, so signing out of A and into B returned
// to a still-selected Settings rendering A's data, against B's session. For the
// delete-account gate that was not a stale label but a way to destroy the wrong
// network, since Api::networkDelete acts on the current JWT and takes no
// arguments.
void MainWindow::LoadCurrentDestination() {
  if (previewUi_) return;  // no session; the relay already said so
  // IsLoggedIn(), not apiReady(): apiReady is api_.has_value(), set at SDK INIT
  // rather than at login, so it is true with no token at all.
  if (!Sdk().IsLoggedIn()) return;
  auto item = HomeNav().SelectedItem().try_as<NavigationViewItem>();
  if (!item) return;
  const auto tag = winrt::unbox_value_or<hstring>(item.Tag(), L"connect");
  if (tag == L"account") {
    account_->LoadAccount();
    Balance().Refresh();  // macOS AccountRootView onAppear parity
  } else if (tag == L"wallet") {
    wallet_->LoadWallet();
  } else if (tag == L"leaderboard") {
    wallet_->LoadLeaderboard();
  } else if (tag == L"settings") {
    settings_->LoadSettings();
  }
}

// ---- balance / plan (SubscriptionBalanceStore relay) -----------------------
// This is the one surface that spans destinations: the same snapshot paints the
// account panel's plan card and the connect drawer's, so it stays at window
// level rather than being duplicated into two pages.

void MainWindow::OnBalanceChanged(urnw::BalanceSnapshot const& snapshot,
                                  urnw::BalancePollState const& poll) {
  balance_ = snapshot;
  balancePoll_ = poll;
  ApplyBalance();
}

void MainWindow::ApplyBalance() {
  // plan value: Guest / Free / Pro (macOS AccountRootView)
  const hstring plan = balance_.guest ? Loc("guest")
                       : balance_.isPro ? Loc("supporter")
                                        : Loc("free");
  AccountPlanValueText().Text(plan);
  DrawerPlanValueText().Text(plan);
  // Pro gold (android theme/Color.kt ProGold). Android spends this colour on the
  // Pro entitlement and on nothing else, so the plan value is the ONE place on
  // these cards that may wear it — until now "Supporter" was a word in the same
  // white as every other label, and the entitlement was invisible. The upgrade
  // affordance beside it stays un-gilded: it is hidden for Pro accounts anyway.
  auto planBrush = balance_.isPro ? urnw::colors::ProGoldBrush()
                                  : urnw::colors::TextBrush();
  AccountPlanValueText().Foreground(planBrush);
  DrawerPlanValueText().Foreground(planBrush);

  // the upgrade affordances show for a signed-in free account; a guest gets a
  // create-account affordance on the plan cards instead (macOS AccountRootView,
  // linux ConnectDrawer), which routes into the guest-upgrade create step
  const auto upgradeVisibility = (!balance_.isPro && !balance_.guest)
                                     ? Visibility::Visible
                                     : Visibility::Collapsed;
  AccountUpgradeButton().Content(
      LocBox(balance_.guest ? "create_an_account" : "upgrade"));
  DrawerGetProButton().Content(
      LocBox(balance_.guest ? "create_an_account" : "get_pro"));
  AccountUpgradeButton().Visibility(balance_.guest ? Visibility::Visible
                                                   : upgradeVisibility);
  DrawerGetProButton().Visibility(balance_.guest ? Visibility::Visible
                                                 : upgradeVisibility);
  // the wallet panel's checkout stays hidden for guests: an account comes first
  UpgradeButton().Visibility(upgradeVisibility);

  // the small ring while the post-checkout confirmation poll runs
  AccountPlanRing().IsActive(balancePoll_.confirming);
  AccountPlanRing().Visibility(balancePoll_.confirming ? Visibility::Visible
                                                       : Visibility::Collapsed);
  DrawerPlanRing().IsActive(balancePoll_.confirming);
  DrawerPlanRing().Visibility(balancePoll_.confirming ? Visibility::Visible
                                                      : Visibility::Collapsed);

  if (accountUsageBar_) {
    accountUsageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount,
                             balance_.availableByteCount);
  }
  if (drawerUsageBar_) {
    drawerUsageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount,
                            balance_.availableByteCount);
  }

  const hstring daily = H(urnw::FormatByteCountCompact(balance_.startBalanceByteCount));
  AccountDailyValue().Text(daily);
  DrawerDailyValue().Text(daily);

  // referral rows: "Total Referrals: N" and "+N*30 GiB/Month"
  const int64_t totalReferrals = account_ ? account_->totalReferrals() : 0;
  const hstring totals = hstring{urnw::Format("total_referrals_lld", totalReferrals)};
  const hstring bonus = hstring{urnw::Format("referral_bonus", totalReferrals * 30)};
  AccountReferralTotals().Text(totals);
  DrawerReferralTotals().Text(totals);
  AccountReferralBonus().Text(bonus);
  DrawerReferralBonus().Text(bonus);

  UpdateBalanceWarning();
  // the open upgrade sheet watches for the plan flip / poll timeout
  if (upgradeSheet_) upgradeSheet_->OnBalance(balance_, balancePoll_);
}

void MainWindow::SetInsufficientBalance(bool insufficient) {
  insufficientBalance_ = insufficient;
  UpdateBalanceWarning();
}

void MainWindow::UpdateBalanceWarning() {
  // macOS ConnectActions: the insufficient-balance CTA shows for a non-Pro
  // account when no confirmation poll is bridging a just-made purchase
  BalanceWarning().IsOpen(insufficientBalance_ && !balance_.isPro &&
                          !balancePoll_.confirming);
  // The hero canvas renders the same two account states (error / processing)
  // off the same fields, so it is re-rendered from the ONE place they change.
  // Guarded: the balance relay can land before the pages are constructed.
  if (connect_) connect_->ApplyConnectStatus();
}

void MainWindow::OnOpenUpgrade(IInspectable const&, RoutedEventArgs const&) {
  // a guest first creates a full account (the plan card's affordance reads
  // "Create an account" for them); checkout is for signed-in free accounts
  if (balance_.guest) {
    login_->BeginGuestUpgrade();
    return;
  }
  ShowUpgradeSheet();
}

void MainWindow::OnOpenRedeem(IInspectable const&, RoutedEventArgs const&) {
  ShowRedeemSheet();
}

winrt::fire_and_forget MainWindow::ShowUpgradeSheet() {
  if (sheetOpen_) co_return;  // only one ContentDialog can show at a time
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->upgradeSheet_ = urnw::UpgradeSheet::Create(Content().XamlRoot(), Sdk(), Balance());
    co_await self->upgradeSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->upgradeSheet_.reset();
  self->sheetOpen_ = false;
}

winrt::fire_and_forget MainWindow::ShowRedeemSheet() {
  if (sheetOpen_) co_return;
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    auto weak = get_weak();
    self->redeemSheet_ = urnw::RedeemCodeSheet::Create(
        Content().XamlRoot(), Sdk(), [weak] {
          // redeemed: poll the balance up (macOS starts the confirmation poll)
          // and refresh the redeemed-codes list
          urnw::App().balance().StartConfirmationPolling();
          if (auto self = weak.get()) self->account().LoadBalanceCodes();
        });
    co_await self->redeemSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->redeemSheet_.reset();
  self->sheetOpen_ = false;
}

// ---- state relay ---------------------------------------------------------

void MainWindow::OnAuthStateChanged(urnw::AuthState state, std::string const& error) {
  ApplyAuthState(state, error);
}

void MainWindow::ApplyAuthState(urnw::AuthState state, std::string const& error) {
  const bool loggedIn = (state == urnw::AuthState::LoggedIn);
  const bool wasVisible = HomeNav().Visibility() == Visibility::Visible;
  // --preview-ui pins the home view; every other branch below still keys off
  // the real auth state, because preview never logs in.
  const bool showHome = loggedIn || previewUi_;
  LoginRoot().Visibility(showHome ? Visibility::Collapsed : Visibility::Visible);
  HomeNav().Visibility(showHome ? Visibility::Visible : Visibility::Collapsed);
  if (!error.empty()) {
    // surface the error on the sign-in step the user is looking at
    login_->ShowErrorOnCurrentStep(H(error));
  }
  if (loggedIn) login_->ClearGuestUpgrade();  // any guest upgrade resolved
  // The network name behind the idle "{name} is ready to connect" copy. Read
  // from the stored jwt once per auth change (ParsedJwt re-parses on every
  // call, and the status line is rewritten on every stats push).
  std::string networkName;
  bool guestMode = false;
  bool pro = false;
  if (loggedIn) {
    if (auto jwt = Sdk().ParsedJwt()) {
      networkName = jwt->NetworkName;
      guestMode = jwt->GuestMode;
      pro = jwt->Pro;
    }
  }
  connect_->SetNetworkIdentity(networkName, guestMode);  // re-renders the status
  // ...and the same identity onto the status strip, which states it on every
  // destination rather than only on Connect.
  if (!statusSamplePinned_) {
    statusSignedIn_ = loggedIn;
    statusNetworkName_ = networkName;
    statusGuest_ = guestMode;
    if (!loggedIn) {
      // a signed-out shell describes no provider and carries no traffic; leave
      // nothing of the previous session's session behind it
      statusLocationName_.clear();
      statusConnected_ = false;
      statusDownBps_ = statusUpBps_ = 0;
    }
    ApplyStatusStrip();
  }
  // The title-bar avatar + its menu (iOS AccountMenu): same jwt, one more
  // reader. `showHome`, NOT `loggedIn` — EnterPreviewUi used to reveal the
  // avatar itself and the very next auth push hid it again, so the one surface
  // that is signed-in-only was the one surface preview could not show. The
  // identity stays whatever the jwt says (empty in preview); only the
  // visibility follows the pinned view.
  login_->ApplyAccountIdentity(networkName, guestMode, pro, showHome);
  if (loggedIn && !wasVisible) {
    // the drawer just appeared: refresh its state and play the entrance
    connect_->ResyncDrawer();
    if (Sdk().IsLoggedIn()) account_->LoadReferralInfo();  // usage-bar referral rows
    if (ConnectView().Visibility() == Visibility::Visible) connect_->AnimateDrawerIn();
    // Whatever destination is still selected from the PREVIOUS session is now
    // showing that session's data against this one's token. Re-read it.
    LoadCurrentDestination();
  }
  if (!loggedIn && wasVisible) {
    // signed out: the flow starts over
    login_->ResetToInitialStep();
    // ...and every page drops the account it was describing. Without this the
    // next sign-in inherits the previous network's name, auth and referral
    // state - which for delete-account and password-reset are acts on the
    // WRONG account, not merely stale text.
    settings_->ResetForSignOut();
    account_->ResetForSignOut();
  }
  if (!loggedIn && !wasVisible && login_->IsGuestUpgrade() && !Sdk().IsLoggedIn()) {
    // the guest session ended under the upgrade form (server-side
    // invalidation): fall back to the start of the sign-in flow
    login_->ClearGuestUpgrade();
    login_->ResetToInitialStep();
  }
}

void MainWindow::OnTunnelStateChanged(urnw::proto::TunnelStatus const& status) {
  connect_->SetConnectedUi(status.state == urnw::proto::TunnelState::Up);
}

void MainWindow::OnStatsChanged(urnw::LiveStats const& stats) {
  connect_->ApplyStats(stats);
  // The status strip's three page-independent facts. The connection half is
  // pushed by ConnectPage out of the SAME ApplyConnectStatus that draws the
  // hero, so the strip and the connect screen cannot disagree about the state.
  //
  // `connected` here is the CLAMPED field: in an rpc-only session it is false
  // and the counters are zero, and the strip says "No traffic yet" rather than
  // printing two honest-looking zero rates under the word Connected.
  if (statusSamplePinned_) return;
  statusConnected_ = stats.connected;
  statusLocationName_ = stats.locationName;
  statusDownBps_ = stats.downBitsPerSecond;
  statusUpBps_ = stats.upBitsPerSecond;
  ApplyStatusStrip();
}

// ---- XAML event handlers ---------------------------------------------------
// The markup binds to these by name; each forwards to the page that owns the
// surface. Keep them one-liners — the behaviour belongs in the page unit.

void MainWindow::OnGetStarted(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnGetStarted(s, e);
}
void MainWindow::OnSignIn(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignIn(s, e);
}
void MainWindow::OnPasswordKeyDown(IInspectable const& s,
                                   Input::KeyRoutedEventArgs const& e) {
  login_->OnPasswordKeyDown(s, e);
}
void MainWindow::OnLoginBack(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnLoginBack(s, e);
}
void MainWindow::OnForgotPassword(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnForgotPassword(s, e);
}
void MainWindow::OnSendResetLink(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSendResetLink(s, e);
}
void MainWindow::OnCreateNameChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnCreateNameChanged(s, e);
}
void MainWindow::OnCreateEmailChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnCreateEmailChanged(s, e);
}
void MainWindow::OnCreatePasswordChanged(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreatePasswordChanged(s, e);
}
void MainWindow::OnTermsChanged(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnTermsChanged(s, e);
}
void MainWindow::OnBonusCodeChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnBonusCodeChanged(s, e);
}
void MainWindow::OnCreateNetwork(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreateNetwork(s, e);
}
void MainWindow::OnVerifyCodeChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnVerifyCodeChanged(s, e);
}
void MainWindow::OnVerifySubmit(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnVerifySubmit(s, e);
}
void MainWindow::OnResendCode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnResendCode(s, e);
}
void MainWindow::OnUseCode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnUseCode(s, e);
}
void MainWindow::OnTryGuestMode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnTryGuestMode(s, e);
}
void MainWindow::OnSignInWithBittensor(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithBittensor(s, e);
}
void MainWindow::OnSignInWithSolana(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithSolana(s, e);
}
void MainWindow::OnUserAuthChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnUserAuthChanged(s, e);
}
void MainWindow::OnSignInWithGoogle(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithGoogle(s, e);
}
void MainWindow::OnSignInWithSeedphrase(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithSeedphrase(s, e);
}
void MainWindow::OnSeedphraseChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnSeedphraseChanged(s, e);
}
void MainWindow::OnSeedphraseSubmit(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSeedphraseSubmit(s, e);
}
void MainWindow::OnCreateInstantAccount(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreateInstantAccount(s, e);
}
void MainWindow::OnInstantTermsChanged(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnInstantTermsChanged(s, e);
}
void MainWindow::OnCreateInstantSubmit(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreateInstantSubmit(s, e);
}
void MainWindow::OnChangeNetworkServer(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnChangeNetworkServer(s, e);
}
void MainWindow::OnAccountMenu(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnAccountMenu(s, e);
}

void MainWindow::OnConnectToggle(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnConnectToggle(s, e);
}
void MainWindow::OnConnectionModeChanged(SelectorBar const& s,
                                         SelectorBarSelectionChangedEventArgs const& e) {
  connect_->OnConnectionModeChanged(s, e);
}
void MainWindow::OnProvideModeChanged(SelectorBar const& s,
                                      SelectorBarSelectionChangedEventArgs const& e) {
  connect_->OnProvideModeChanged(s, e);
}
void MainWindow::OnFixedIpToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnFixedIpToggled(s, e);
}
void MainWindow::OnStrongAnonToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnStrongAnonToggled(s, e);
}
void MainWindow::OnPostQuantumToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnPostQuantumToggled(s, e);
}
void MainWindow::OnBlockerToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnBlockerToggled(s, e);
}
void MainWindow::OnClientStatsCardClick(IInspectable const& s,
                                         RoutedEventArgs const& e) {
  connect_->OnClientStatsCardClick(s, e);
}
void MainWindow::OnLocalStatsCardClick(IInspectable const& s,
                                        RoutedEventArgs const& e) {
  connect_->OnLocalStatsCardClick(s, e);
}
void MainWindow::OnDnsCardClick(IInspectable const& s,
                                 RoutedEventArgs const& e) {
  connect_->OnDnsCardClick(s, e);
}
void MainWindow::OnLocationRowClick(IInspectable const& s,
                                     RoutedEventArgs const& e) {
  connect_->OnLocationRowClick(s, e);
}
void MainWindow::OnPeersLineClick(IInspectable const& s,
                                   RoutedEventArgs const& e) {
  connect_->OnPeersLineClick(s, e);
}

void MainWindow::OnSaveNetworkName(IInspectable const& s, RoutedEventArgs const& e) {
  account_->OnSaveNetworkName(s, e);
}

void MainWindow::OnWalletAddressChanged(IInspectable const& s,
                                        TextChangedEventArgs const& e) {
  wallet_->OnWalletAddressChanged(s, e);
}
void MainWindow::OnConnectWallet(IInspectable const& s, RoutedEventArgs const& e) {
  wallet_->OnConnectWallet(s, e);
}
void MainWindow::OnVerifySeeker(IInspectable const& s, RoutedEventArgs const& e) {
  wallet_->OnVerifySeeker(s, e);
}
void MainWindow::OnLeaderboardPublicToggled(IInspectable const& s, RoutedEventArgs const& e) {
  wallet_->OnLeaderboardPublicToggled(s, e);
}

void MainWindow::OnManageAppSplitTunnel(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnManageAppSplitTunnel(s, e);
}
void MainWindow::OnSignOut(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnSignOut(s, e);
}
void MainWindow::OnSendFeedback(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnSendFeedback(s, e);
}

}  // namespace winrt::URnetwork::implementation
