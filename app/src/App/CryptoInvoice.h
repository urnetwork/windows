// The sdk's CryptoInvoiceViewController for the signed-in session (crypto-pay
// CONTRACT §4.4): the app's owner of its one native crypto invoice. The sdk
// does the work -- the quote, the countdown, the requote, the status poll and
// the invoice saved on this device -- and this store opens the controller at
// sign-in, resumes the saved invoice, runs the controller only while an
// invoice is on screen in a window that presents (no background polling,
// ZCASH.md §0.3), and publishes the controller's state as a
// cryptopay::CryptoInvoiceView.
//
// AppController owns it, as it owns SubscriptionBalanceStore, and forwards its
// changes to the window, which paints the Account rows and the open upgrade
// sheet from them (MainWindow::OnCryptoInvoiceChanged).
//
// The controller keeps the saved invoice and the device's last payment method
// in the store's own directory (StorageDir), not in the sdk's local state,
// because the C ABI does not hand the app that directory; the store empties
// its directory at sign-out, as the sdk's local state is emptied then.
//
// Threading: every public method runs on the UI thread. The sdk calls the
// change listener from its own threads; the listener only enqueues onto the UI
// queue, and the state is read there, under a generation that every open and
// close of the controller moves on, so a change queued for a controller that
// is gone is dropped.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "CryptoPayPresentation.h"
#include "SdkHost.h"

namespace urnw {

class CryptoInvoiceStore {
 public:
  using ChangeHandler = std::function<void(cryptopay::CryptoInvoiceView const&)>;

  explicit CryptoInvoiceStore(SdkHost& sdk) : sdk_(sdk) {}
  ~CryptoInvoiceStore();
  CryptoInvoiceStore(CryptoInvoiceStore const&) = delete;
  CryptoInvoiceStore& operator=(CryptoInvoiceStore const&) = delete;

  // The UI thread's queue, for the listener's hops. Call once, before Start().
  void Initialize(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue);
  // Runs on the UI thread after every change of the state.
  void SetChangeHandler(ChangeHandler handler) { onChange_ = std::move(handler); }

  // Sign-in (and the launch of a signed-in app): opens the controller on this
  // session's Api and resumes the invoice saved for this network, if any. A
  // repeated sign-in push (a device re-registration) opens a fresh one.
  void Start();
  // The session ends: the listener goes, then the controller closes. At
  // sign-out (`signedOut`) the saved invoice and the remembered method go too;
  // quitting the app keeps them for the next launch.
  void Stop(bool signedOut);

  // The window presents (AppController::ReconcileWindowPresentation).
  void SetPresenting(bool presenting);
  // An upgrade sheet shows the invoice, or is starting one. The controller
  // runs (polls and ticks) only while both hold.
  void SetShowing(bool showing);

  // The controller's actions (§4.4). Each is a no-op without a controller.
  // StartInvoice answers false when the start could not even be asked for (no
  // controller, or the sdk threw); its outcome otherwise arrives as a state.
  bool StartInvoice(std::string const& asset, std::string const& plan);
  // The invoice saved for the signed-in network back on the store, at launch
  // and when the Account page appears. False when there is none.
  bool Resume();
  void Requote();
  // "Pay another way" (offered only while nothing was received, or after a
  // failure): the invoice is forgotten, or only put away while a payment sent
  // within its quote may still confirm (the sdk's awaiting_confirmation,
  // Bitcoin's grace, mmm/BTCPAY.md §16.8), and then it comes back at the next
  // Resume. A sheet's Close does not dismiss; it only stops showing the
  // invoice, which stays saved (CONTRACT §6.2.1).
  void Dismiss();
  // The last method chosen on this device ("" when none), and a new choice.
  std::string PaymentMethod() const;
  void SetPaymentMethod(std::string const& method);

  // A controller is open (signed in, and the sdk opened one).
  bool Ready() const { return controller_.has_value(); }
  cryptopay::CryptoInvoiceView const& Current() const { return view_; }

 private:
  // Where the controller keeps the saved invoice and the remembered method.
  static std::filesystem::path StorageDir();
  void Close();
  // The controller's state onto view_, and publish it.
  void Read();
  // Start or stop the controller to match presenting_ && showing_.
  void ApplyRunning();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
  ChangeHandler onChange_;
  std::optional<urnet::CryptoInvoiceViewController> controller_;
  std::optional<urnet::Sub> sub_;
  // moves on at every open and close: a change queued for a closed controller
  // is dropped (UI thread only)
  uint64_t generation_ = 0;
  // false once the store is destroyed: a change still queued stops here
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  bool presenting_ = false;
  bool showing_ = false;
  bool running_ = false;  // what the controller was last told
  cryptopay::CryptoInvoiceView view_;
};

}  // namespace urnw
