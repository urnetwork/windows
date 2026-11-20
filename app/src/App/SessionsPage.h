// The Account > Sessions page (SessionsView in MainWindow.xaml): the account's
// signed-in sessions, and signing them out (server session/REVOKE-UI-FINAL.md;
// android, apple, linux and ur.io build the same screen on the same
// controller). Reached from the Account list's Sessions row and shown in
// Account's place, like the Refer and earn page; "‹ Account" and any rail
// navigation close it.
//
// The sdk's ClientSessionViewController does the work: it lists the sessions
// (every page of them), sorts them into the device sign-ins and the client
// groups below them (hosted proxies, API clients, paired devices, other
// clients) with their counts, polls every 30 seconds while it is visible,
// carries each revoke's operation id through retries and a 202's pending
// state, refuses a response for an older login, and after a confirmed sign-out
// of this session rejects the credential, which SdkHost's Api logout listener
// turns into the app's normal sign-out. The page opens one on the in-process
// Api when it opens and closes it when it closes; in between it tells it when
// it is visible (the page shown and the window presenting), when the window
// comes to the foreground, which groups are open and when a section shows
// more, and renders what it publishes. SessionsPresentation.h decides
// everything before the XAML.
//
// Threading is WalletPage's provider status: the controller publishes on its
// own thread, where the listener reads the snapshot into plain values; the
// value is marshalled to the UI thread through the DispatcherQueue, resolved
// through the window's weak reference, and dropped once the page is destroyed
// (alive_) or the controller it came from is closed (fence_). Everything else
// here runs on the UI thread, and every call it makes into the controller
// returns at once: the controller's requests run on its own goroutines. Opening
// or closing a group and Show more are presentation only and request nothing;
// the controller publishes their snapshot before it returns, on this thread,
// where the listener reads it (the rows shown, a page of 50 at a time) and
// queues it like any other.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Sdk.h"  // urnet::ClientSessionViewController, urnet::Sub
#include "SessionsPresentation.h"
#include "UrComponents.h"  // kit::Snackbar

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class SessionsPage {
 public:
  explicit SessionsPage(winrt::URnetwork::implementation::MainWindow& window);
  // closes the controller; a snapshot still queued for the page is dropped
  ~SessionsPage();

  SessionsPage(SessionsPage const&) = delete;
  SessionsPage& operator=(SessionsPage const&) = delete;

  // the pane title, the back affordance and Refresh; the rows' words follow
  void ApplyStrings();
  // MainWindow::OpenSessions: the page shows. Opens a controller on the
  // in-process Api and starts it; with no session (signed out, --preview-ui)
  // it opens none and says so.
  void Open();
  // MainWindow::CloseSessions, a rail navigation, a sign-out: the listener is
  // dropped, then the controller is closed, and the list goes with it.
  void Close();
  // the window presents or stops (MainWindow::SetPresentationActive): the
  // controller polls only while the page is shown and the window presents,
  // and a return to the foreground refreshes
  void SetPresentationActive(bool active);
  // a sign-out: nothing of the departed account stays on the page
  void ResetForSignOut();

 private:
  // the body's controls a rebuild puts keyboard focus back on
  enum class FocusKind { None, SignOut, Copy, Bulk, TryAgain, GroupHeader, ShowMore };
  struct FocusTarget {
    FocusKind kind = FocusKind::None;
    std::string sessionId;
    // the section of the row, the group of the heading, or the section of the
    // Show more: a group's kind, or sessions::kGroupSignIns
    std::string section;
  };
  // one row's controls, by the session they act on
  struct RowControls {
    std::string sessionId;
    winrt::Microsoft::UI::Xaml::Controls::Button signOut{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button copy{nullptr};
    // its section: its group's kind, or sessions::kGroupSignIns
    std::string section;
  };
  // one section's controls: a group's heading (none for the device sign-ins)
  // and its Show more, when it has one
  struct SectionControls {
    std::string kind;
    winrt::Microsoft::UI::Xaml::Controls::Button header{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button showMore{nullptr};
  };

  void Build();  // idempotent: the header's handlers
  void OpenController();
  void CloseController();
  // SetVisible from the page shown and the window presenting, on a change
  void ApplyVisibility();
  // a snapshot from the controller opened as `generation`, on the UI thread
  void ApplySnapshot(uint64_t generation, sessions::Snapshot snapshot);
  // the header's indicator always; the body when what it shows changed (or
  // `force`), since a rebuild moves keyboard focus
  void Render(bool force);
  void RenderBody(sessions::View const& view);
  void AppendRow(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
                 sessions::Row const& row);
  // Show more under the section `kind`; `section` names the section for a
  // screen reader ("" for the device sign-ins, which have no heading)
  void AppendShowMore(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
                      std::string const& kind, winrt::hstring const& section);
  // a client group: its heading, then while it is open its rows and Show more
  void AppendGroup(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
                   sessions::GroupView const& group);
  void AppendBulk(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
                  sessions::View const& view);
  SectionControls& SectionFor(std::string const& kind);
  FocusTarget FocusedTarget() const;
  void RestoreFocus(FocusTarget const& target);
  // the header's Refresh and the failed load's Try again
  void Refresh();
  // a group's heading: opens or closes it (SetGroupExpanded)
  void SetGroupExpanded(std::string const& kind, bool expanded);
  // a section's Show more: its next rows (ShowMoreSessions)
  void ShowMore(std::string const& kind);
  void CopyId(std::string const& sessionId);
  // every sign-out asks first (§4); Cancel is the default
  winrt::fire_and_forget ConfirmSignOut(std::string sessionId);
  winrt::fire_and_forget ConfirmSignOutOthers();
  // the confirmed revokes, through the controller the dialog opened over
  void SignOut(std::string const& sessionId, uint64_t generation);
  void SignOutOthers(uint64_t generation);

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Copied!"
  urnw::kit::Snackbar snackbar_;
  bool built_ = false;
  // the page is up in Account's place
  bool open_ = false;
  // the window presents
  bool presentationActive_ = false;
  // what SetVisible last told the controller
  std::optional<bool> visibleSent_;

  std::optional<urnet::ClientSessionViewController> controller_;
  std::optional<urnet::Sub> sub_;
  sessions::OpenFence fence_;
  // the controller's last snapshot, and the body on screen from it
  sessions::Snapshot snapshot_;
  std::optional<sessions::View> rendered_;
  // false once the page is destroyed: a queued snapshot stops here
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  std::vector<RowControls> rowControls_;
  std::vector<SectionControls> sectionControls_;
  winrt::Microsoft::UI::Xaml::Controls::Button bulkButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button tryAgainButton_{nullptr};
};

}  // namespace urnw
