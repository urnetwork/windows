// Executable spec for how Earnings words the one-tap Bittensor key and its
// claims (App/BittensorKeyPresentation.h) and for the bytes of the key's store
// (App/BittensorKeyStoreFormat.h): the disclaimer the sdk names as the owner's
// two store keys and nothing else, each failure in the store's words, the
// payout card's body for every state of the sdk's card and the consent read,
// the Key screen's steps and texts, the reminder's step, the claim relay's
// reasons and notes, the amounts, and the store's file names, items and notes.
// Run against the same headers the app compiles, on any host with a C++20
// compiler.
//
// The WinUI half (the card's views, the Key screen, Windows Hello, the capture
// exclusion, the clipboard, DPAPI) cannot be built off Windows; what is checked
// here is every choice it makes before it touches a XAML object or Win32.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App bittensor-key-tests.cpp -o /tmp/bittensor-key-tests && /tmp/bittensor-key-tests
//
// With URNW_BITTENSOR_KEY_TESTS_SDK each id mirrored in the header is compared
// with the generated SDK header's own (urnetwork_sdk.hpp), so a renamed state,
// failure, reason or disclaimer id fails here first. The header needs
// nlohmann/json; both are system includes because the generated code does not
// build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_BITTENSOR_KEY_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> bittensor-key-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "BittensorKeyPresentation.h"
#include "BittensorKeyStoreFormat.h"

#if defined(URNW_BITTENSOR_KEY_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckEq(std::string_view expected, std::string_view actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + std::string(expected) + "\", got \"" + std::string(actual) + "\"");
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
// several cases a function: the line number names each one
#define URNW_CONCAT_INNER(a, b) a##b
#define URNW_CONCAT(a, b) URNW_CONCAT_INNER(a, b)
#define TEST_CASE(name) Case URNW_CONCAT(case_, __LINE__)(name)

const char* BodyName(btkey::CardBody body) {
  switch (body) {
    case btkey::CardBody::WalletCard: return "WalletCard";
    case btkey::CardBody::SetUp: return "SetUp";
    case btkey::CardBody::FinishSetup: return "FinishSetup";
    case btkey::CardBody::StoreUnavailable: return "StoreUnavailable";
    case btkey::CardBody::Linked: return "Linked";
    case btkey::CardBody::Deleted: return "Deleted";
    case btkey::CardBody::Elsewhere: return "Elsewhere";
  }
  return "?";
}

void CheckLayout(btkey::ConsentLine line, std::string_view state, bool showsKeyPath, btkey::CardBody body,
                 bool otherConsentLines, bool storeUnavailableLines, const std::string& what) {
  const btkey::CardLayout layout = btkey::CardLayoutFor(line, state, showsKeyPath);
  CheckEq(BodyName(body), BodyName(layout.body), what + ": the body");
  Check(layout.otherConsentLines == otherConsentLines, what + ": the other consent's lines");
  Check(layout.storeUnavailableLines == storeUnavailableLines, what + ": the store's lines");
}

void TestDisclaimer() {
  TEST_CASE("the disclaimer is the owner's two store keys, as the sdk names them, and nothing else");
  CheckEq("bittensor_funds_disclaimer", btkey::DisclaimerKey("bittensor_funds_disclaimer"), "the whole text");
  CheckEq("bittensor_funds_disclaimer_funds_only", btkey::DisclaimerKey("bittensor_funds_disclaimer_funds_only"),
          "the last sentence");
  CheckEq("", btkey::DisclaimerKey(""), "none");
  CheckEq("", btkey::DisclaimerKey("bittensor_funds_disclaimer_reworded"), "an id the sdk never names");
  CheckEq("", btkey::DisclaimerKey("sn_consent_body"), "another key");
}

void TestFailures() {
  TEST_CASE("each failure the flow reports reads in the store's words");
  struct Row {
    const char* code;
    const char* message;
    const char* key;
    const char* shown;
  };
  for (const Row& row : std::vector<Row>{
           {"", "", "", ""},
           {"challenge_expired", "challenge_expired: late", "sn_consent_expired", ""},
           {"signature_mismatch", "nope", "sn_consent_failed", ""},
           {"network_owner_only", "network_owner_only: no", "sn_consent_owner_only", ""},
           {"not_allowed", "", "sn_consent_owner_only", ""},
           {"generate_failed", "", "bittensor_key_failure_generate", ""},
           {"store_failed", "", "bittensor_key_failure_store", ""},
           {"store_unavailable", "", "bittensor_key_failure_keychain", ""},
           {"delete_failed", "", "bittensor_key_failure_delete", ""},
           {"replacement_not_confirmed", "", "bittensor_key_replace_message", ""},
           {"consent_failed", "", "bittensor_key_failure_consent", ""},
           {"consent_failed", "the server said no", "", "the server said no"},
           {"something_new", "a new server state", "", "a new server state"},
       }) {
    const btkey::FailureText text = btkey::FailureTextFor(row.code, row.message);
    CheckEq(row.key, text.key, std::string("the key for ") + row.code);
    CheckEq(row.shown, text.message, std::string("the message for ") + row.code);
  }
  Check(btkey::FailureTextFor("", "ignored").empty(), "no code is no failure, whatever the message");
}

void TestCardLayout() {
  using btkey::CardBody;
  using btkey::ConsentLine;
  TEST_CASE("no consent: the key's path while the sdk shows it, the wallet card otherwise");
  CheckLayout(ConsentLine::None, "none", true, CardBody::SetUp, false, false, "1a: no key, set up offered");
  CheckLayout(ConsentLine::None, "none", false, CardBody::WalletCard, false, false,
              "a guest or a session that isn't the owner's");
  CheckLayout(ConsentLine::None, "save_later_not_linked", true, CardBody::FinishSetup, false, false,
              "3i: setup cut short");
  CheckLayout(ConsentLine::None, "store_unavailable", true, CardBody::StoreUnavailable, false, false,
              "the store can't answer");
  CheckLayout(ConsentLine::None, "save_now_showing", false, CardBody::WalletCard, false, false,
              "Save now: the Key screen is up");

  TEST_CASE("a consent: the key's body for a key the consent pays, made here, or elsewhere");
  CheckLayout(ConsentLine::Active, "save_later_linked", false, CardBody::Linked, false, false, "1c, 1i");
  CheckLayout(ConsentLine::Active, "saved_and_deleted", false, CardBody::Deleted, false, false, "1l, 2h");
  CheckLayout(ConsentLine::Active, "elsewhere", false, CardBody::Elsewhere, false, false, "3a, 3d");
  CheckLayout(ConsentLine::Active, "save_later_not_linked", true, CardBody::WalletCard, true, false,
              "a key here under another consent: the wallet card's lines, and the key's under them");
  CheckLayout(ConsentLine::Active, "store_unavailable", true, CardBody::WalletCard, false, true,
              "a consent, and a store that can't answer");
  CheckLayout(ConsentLine::Active, "loading", false, CardBody::WalletCard, false, false, "not read yet");

  TEST_CASE("an unknown consent read draws no key body: the header says loading or failed");
  for (std::string_view state : {"none", "save_later_linked", "saved_and_deleted", "elsewhere",
                                 "save_later_not_linked", "store_unavailable"}) {
    CheckLayout(ConsentLine::Unknown, state, true, CardBody::WalletCard, false, false, std::string(state));
  }

  TEST_CASE("the title of a card the consent pays says when payouts start");
  CheckEq("bittensor_key_payouts_start", btkey::PayoutsTitleKey(true), "next epoch");
  CheckEq("bittensor_key_payouts_on", btkey::PayoutsTitleKey(false), "on");
}

void TestKeyScreen() {
  TEST_CASE("one step at a time: the two words, what deleting means, the screen");
  Check(btkey::KeyStepFor("", false) == btkey::KeyStep::Main, "the screen");
  Check(btkey::KeyStepFor("", true) == btkey::KeyStep::DeleteExplained, "what deleting means");
  Check(btkey::KeyStepFor("delete", true) == btkey::KeyStep::Confirm, "the words win once the sdk asks");
  Check(btkey::KeyStepFor("delete", false) == btkey::KeyStep::Confirm, "Delete's words");
  Check(btkey::KeyStepFor("keep", false) == btkey::KeyStep::Confirm, "Keep's words");

  TEST_CASE("the banner: signing first, then Retry, then the Save now notice");
  Check(btkey::KeyBannerFor(true, true, true) == btkey::KeyBanner::Signing, "busy");
  Check(btkey::KeyBannerFor(false, true, true) == btkey::KeyBanner::Retry, "a refused consent");
  Check(btkey::KeyBannerFor(false, false, true) == btkey::KeyBanner::SaveNow, "Save now");
  Check(btkey::KeyBannerFor(false, false, false) == btkey::KeyBanner::None, "a stored key");

  TEST_CASE("what deleting means: a stored key's bullets, and Save now's");
  const btkey::DeleteText stored = btkey::DeleteTextFor(false);
  CheckEq("bittensor_key_delete_title_2", stored.titleKey, "a stored key's title");
  Check(stored.bullets.size() == 4, "four bullets");
  if (stored.bullets.size() == 4) {
    CheckEq("bittensor_key_delete_bullet_no_recovery", stored.bullets[0].key, "first");
    CheckEq("bittensor_key_delete_bullet_address", stored.bullets[1].key, "second");
    Check(stored.bullets[1].namesAddress && !stored.bullets[0].namesAddress, "only the address bullet names it");
    CheckEq("bittensor_key_delete_bullet_phrase", stored.bullets[2].key, "third");
    CheckEq("bittensor_key_delete_bullet_convert", stored.bullets[3].key, "fourth");
  }
  const btkey::DeleteText saveNow = btkey::DeleteTextFor(true);
  CheckEq("bittensor_key_delete_save_now_title", saveNow.titleKey, "Save now's title");
  Check(saveNow.bullets.size() == 4, "four bullets on Save now");
  if (saveNow.bullets.size() == 4) {
    CheckEq("bittensor_key_save_now_bullet_address", saveNow.bullets[0].key, "first on Save now");
    Check(saveNow.bullets[0].namesAddress, "it names the address");
    CheckEq("bittensor_key_delete_bullet_no_recovery", saveNow.bullets[1].key, "second on Save now");
    CheckEq("bittensor_key_save_now_bullet_phrase", saveNow.bullets[2].key, "third on Save now");
    CheckEq("bittensor_key_save_now_bullet_convert", saveNow.bullets[3].key, "fourth on Save now");
  }

  TEST_CASE("the two words: Keep's and Delete's texts and buttons");
  const btkey::ConfirmText keep = btkey::ConfirmTextFor("keep", false);
  CheckEq("bittensor_key_keep_title_2", keep.titleKey, "Keep's title");
  CheckEq("bittensor_key_keep_body", keep.bodyKey, "Keep's body");
  CheckEq("bittensor_key_keep_prompt", keep.promptKey, "Keep's prompt");
  CheckEq("bittensor_key_keep_confirm_2", keep.buttonKey, "Keep's button");
  Check(!keep.destructive, "Keep is not destructive");
  CheckEq("bittensor_key_keep_save_now_body", btkey::ConfirmTextFor("keep", true).bodyKey, "Keep on Save now");
  const btkey::ConfirmText remove = btkey::ConfirmTextFor("delete", false);
  CheckEq("bittensor_key_enter_two_words", remove.titleKey, "Delete's title");
  CheckEq("", remove.bodyKey, "Delete has no body");
  CheckEq("bittensor_key_enter_two_words_body", remove.promptKey, "Delete's prompt");
  CheckEq("bittensor_key_delete_confirm", remove.buttonKey, "Delete's button");
  Check(remove.destructive, "Delete is coral");
  CheckEq("bittensor_key_save_now_delete_confirm", btkey::ConfirmTextFor("delete", true).buttonKey,
          "Save now's Delete sets up payouts first");

  TEST_CASE("the countdown to the words hiding, rounded up, never below 0:00");
  CheckEq("1:00", btkey::Countdown(60'000, 0), "a whole minute");
  CheckEq("0:58", btkey::Countdown(58'000, 0), "seconds");
  CheckEq("0:58", btkey::Countdown(57'001, 0), "rounded up");
  CheckEq("0:01", btkey::Countdown(1, 0), "the last moment");
  CheckEq("0:00", btkey::Countdown(1'000, 1'000), "hiding now");
  CheckEq("0:00", btkey::Countdown(0, 5'000), "past");
  CheckEq("2:05", btkey::Countdown(1'125'000, 1'000'000), "minutes and seconds");
}

void TestReminder() {
  TEST_CASE("the reminder's step: the app posts it itself, and reports only what went out");
  Check(btkey::ReminderStepFor("schedule") == btkey::ReminderStep::Arm, "a schedule is waited for, not reported");
  Check(btkey::ReminderStepFor("deliver_now") == btkey::ReminderStep::PostNow, "deliver now");
  Check(btkey::ReminderStepFor("cancel") == btkey::ReminderStep::Disarm, "cancel");
  Check(btkey::ReminderStepFor("none") == btkey::ReminderStep::Nothing, "none");
  Check(btkey::ReminderStepFor("something_new") == btkey::ReminderStep::Nothing, "an action this build doesn't know");
}

void TestRelay() {
  TEST_CASE("URnetwork is the one claiming unless it isn't running or has paused");
  for (std::string_view reason : {"nothing_pending", "under_threshold", "queued", "cap_reached", "refused"}) {
    Check(btkey::RelayClaims(reason), std::string(reason) + " is the relay's");
  }
  Check(!btkey::RelayClaims("relay_not_running"), "not running");
  Check(!btkey::RelayClaims("relay_paused"), "paused");

  TEST_CASE("why a balance waits, in the store's words");
  struct Row {
    const char* reason;
    int64_t thresholdTaoRao;
    int64_t thresholdRao;
    const char* title;
    btkey::ReasonDetail detail;
  };
  for (const Row& row : std::vector<Row>{
           {"under_threshold", 12'500'000, 3'000'000'000, "sn_claim_relay_under_threshold", btkey::ReasonDetail::Threshold},
           {"under_threshold", 12'500'000, 0, "sn_claim_relay_under_threshold", btkey::ReasonDetail::ThresholdNoPrice},
           {"under_threshold", 0, 0, "sn_claim_relay_under_threshold", btkey::ReasonDetail::None},
           {"queued", 0, 0, "sn_claim_relay_queued", btkey::ReasonDetail::None},
           {"cap_reached", 0, 0, "sn_claim_relay_cap", btkey::ReasonDetail::None},
           {"refused", 0, 0, "sn_claim_relay_refused", btkey::ReasonDetail::None},
           {"relay_not_running", 0, 0, "sn_claim_relay_not_running", btkey::ReasonDetail::Waits},
           {"relay_paused", 0, 0, "sn_claim_relay_paused", btkey::ReasonDetail::Waits},
           {"nothing_pending", 0, 0, "", btkey::ReasonDetail::None},
           {"something_new", 0, 0, "", btkey::ReasonDetail::None},
       }) {
    const btkey::ReasonText text = btkey::ReasonTextFor(row.reason, row.thresholdTaoRao, row.thresholdRao);
    CheckEq(row.title, text.titleKey, std::string("the title for ") + row.reason);
    Check(text.detail == row.detail, std::string("the detail for ") + row.reason);
  }

  TEST_CASE("under Claim now: who pays and how often, or why it can't act");
  using btkey::ClaimNowNote;
  Check(btkey::ClaimNowNoteFor(true, 30, "", 0, 0, false) == ClaimNowNote::Paid, "the tile's");
  Check(btkey::ClaimNowNoteFor(true, 30, "", 0, 0, true) == ClaimNowNote::PaidEach, "the sheet's, per coldkey");
  Check(btkey::ClaimNowNoteFor(true, 0, "", 0, 0, false) == ClaimNowNote::None, "no period, no note");
  Check(btkey::ClaimNowNoteFor(false, 30, "under_floor", 100'000, 0, false) == ClaimNowNote::UnderFloor, "the floor");
  Check(btkey::ClaimNowNoteFor(false, 30, "under_floor", 0, 0, false) == ClaimNowNote::None, "no floor figure");
  Check(btkey::ClaimNowNoteFor(false, 30, "requested", 0, 0, false) == ClaimNowNote::Requested, "requested");
  Check(btkey::ClaimNowNoteFor(false, 30, "limit", 0, 1'800'000'000'000, false) == ClaimNowNote::AgainOn, "the limit");
  Check(btkey::ClaimNowNoteFor(false, 30, "limit", 0, 0, false) == ClaimNowNote::None, "no date");
  Check(btkey::ClaimNowNoteFor(false, 30, "queued", 0, 0, false) == ClaimNowNote::None, "another reason");

  TEST_CASE("in the claimed state the line says when Claim now is available again, and nothing else");
  Check(btkey::ClaimedNoteFor(1'800'000'000'000) == ClaimNowNote::AgainOn, "the claimed state's limit");
  Check(btkey::ClaimedNoteFor(0) == ClaimNowNote::None, "no limit, no line");

  TEST_CASE("amounts: TAO to four places, SN25α as precise as its size needs");
  CheckEq("0.0125", btkey::TaoAmount(12'500'000), "the automatic threshold");
  CheckEq("0.0001", btkey::TaoAmount(100'000), "the transfer floor");
  CheckEq("0.0000", btkey::TaoAmount(0), "zero");
  CheckEq("30", btkey::ApproxAlpha(30'000'000'000), "ten or more");
  CheckEq("3.0", btkey::ApproxAlpha(3'000'000'000), "one to ten");
  CheckEq("1.5", btkey::ApproxAlpha(1'500'000'000), "one and a half");
  CheckEq("0.0420", btkey::ApproxAlpha(42'000'000), "under one");
}

void TestStoreFormat() {
  TEST_CASE("a network's files are named by its id, and no other id reaches the folder");
  CheckEq("2f0b6c1e-9a4d-4e7b-8c55-0123456789ab",
          btkeystore::FileStem("2f0b6c1e-9a4d-4e7b-8c55-0123456789ab").value_or("<refused>"), "a uuid");
  for (std::string_view id : {"", "..", "../other", "a/b", "a\\b", "C:", "name.key", "a b", "id\n",
                              "\xc3\xa9t\xc3\xa9"}) {
    Check(!btkeystore::FileStem(id).has_value(), "refused: " + std::string(id));
  }
  Check(!btkeystore::FileStem(std::string(65, 'a')).has_value(), "too long");
  Check(btkeystore::FileStem(std::string(64, 'a')).has_value(), "the longest");

  TEST_CASE("the phrase's entropy belongs to its network");
  Check(btkeystore::Entropy("a") != btkeystore::Entropy("b"), "two networks, two entropies");
  Check(btkeystore::Entropy("a") == btkeystore::Entropy("a"), "the same network's again");
  Check(btkeystore::Entropy("a").find("URnetwork Windows Bittensor key v1") == 0, "the store's constant first");

  TEST_CASE("an item round-trips, and anything else is not one");
  const btkeystore::Item item{"5FakeAddressOfTheStoreFormatSpec000000000000000", std::string("\x01\x00\x0a\xff", 4)};
  const auto bytes = btkeystore::EncodeItem(item);
  Check(bytes.has_value(), "it encodes");
  if (bytes) {
    Check(bytes->starts_with("URNETBTKEY1\n"), "the magic first");
    const auto decoded = btkeystore::DecodeItem(*bytes);
    Check(decoded.has_value() && *decoded == item, "it decodes to itself, a blob with a newline in it included");
    Check(!btkeystore::DecodeItem(bytes->substr(1)).has_value(), "no magic");
    Check(!btkeystore::DecodeItem("URNETBTKEY1\nno-newline").has_value(), "no address line");
    Check(!btkeystore::DecodeItem("URNETBTKEY1\n5F3s\n").has_value(), "no blob");
    Check(!btkeystore::DecodeItem("URNETBTKEY1\n5F 3s\nblob").has_value(), "an address that isn't one");
  }
  Check(!btkeystore::EncodeItem({"", "blob"}).has_value(), "no address");
  Check(!btkeystore::EncodeItem({"5F3s\n", "blob"}).has_value(), "an address with a line break");
  Check(!btkeystore::EncodeItem({"5F3s", ""}).has_value(), "no protected phrase");
  Check(!btkeystore::DecodeItem(std::string(btkeystore::kMaxFileBytes + 1, 'x')).has_value(), "too big");

  TEST_CASE("the notes: written, read back, removed by an empty value, and never more than a line");
  btkeystore::Notes notes;
  Check(btkeystore::WriteNote(notes, "saved_address", "5F3s"), "a note");
  Check(btkeystore::WriteNote(notes, "reminder_at_millis", "1800000000000"), "another");
  const btkeystore::Notes read = btkeystore::DecodeNotes(btkeystore::EncodeNotes(notes));
  Check(read == notes, "they round-trip");
  Check(btkeystore::WriteNote(notes, "saved_address", ""), "an empty value");
  Check(notes.count("saved_address") == 0 && notes.size() == 1, "removes the note");
  Check(!btkeystore::WriteNote(notes, "evil\tname", "x"), "a tab in the name");
  Check(!btkeystore::WriteNote(notes, "name", "two\nlines"), "a line break in the value");
  Check(!btkeystore::WriteNote(notes, "", "x"), "no name");
  Check(notes.size() == 1, "a refused note changes nothing");
  const btkeystore::Notes junk = btkeystore::DecodeNotes("no tab here\n\tno name\nempty\t\nok\tvalue\n");
  Check(junk.size() == 1 && junk.count("ok") == 1 && junk.at("ok") == "value", "lines that aren't notes are dropped");
}

#if defined(URNW_BITTENSOR_KEY_TESTS_SDK)
void TestAgainstSdkHeader() {
  TEST_CASE("every id the header mirrors is the generated SDK header's own");
  struct Pair {
    std::string_view mirrored;
    std::string_view sdk;
    const char* name;
  };
  for (const Pair& pair : std::vector<Pair>{
           {btkey::kStateLoading, urnet::BittensorLocalKeyStateLoading, "StateLoading"},
           {btkey::kStateConsentUnavailable, urnet::BittensorLocalKeyStateConsentUnavailable, "StateConsentUnavailable"},
           {btkey::kStateStoreUnavailable, urnet::BittensorLocalKeyStateStoreUnavailable, "StateStoreUnavailable"},
           {btkey::kStateNone, urnet::BittensorLocalKeyStateNone, "StateNone"},
           {btkey::kStateSaveNowShowing, urnet::BittensorLocalKeyStateSaveNowShowing, "StateSaveNowShowing"},
           {btkey::kStateSaveNowConsentFailed, urnet::BittensorLocalKeyStateSaveNowConsentFailed, "StateSaveNowConsentFailed"},
           {btkey::kStateSaveLaterNotLinked, urnet::BittensorLocalKeyStateSaveLaterNotLinked, "StateSaveLaterNotLinked"},
           {btkey::kStateSaveLaterLinked, urnet::BittensorLocalKeyStateSaveLaterLinked, "StateSaveLaterLinked"},
           {btkey::kStateSavedAndDeleted, urnet::BittensorLocalKeyStateSavedAndDeleted, "StateSavedAndDeleted"},
           {btkey::kStateElsewhere, urnet::BittensorLocalKeyStateElsewhere, "StateElsewhere"},
           {btkey::kFailureGenerate, urnet::BittensorLocalKeyFailureGenerate, "FailureGenerate"},
           {btkey::kFailureStore, urnet::BittensorLocalKeyFailureStore, "FailureStore"},
           {btkey::kFailureStoreUnavailable, urnet::BittensorLocalKeyFailureStoreUnavailable, "FailureStoreUnavailable"},
           {btkey::kFailureDelete, urnet::BittensorLocalKeyFailureDelete, "FailureDelete"},
           {btkey::kFailureNotAllowed, urnet::BittensorLocalKeyFailureNotAllowed, "FailureNotAllowed"},
           {btkey::kFailureReplacementNotConfirmed, urnet::BittensorLocalKeyFailureReplacementNotConfirmed,
            "FailureReplacementNotConfirmed"},
           {btkey::kFailureConsent, urnet::BittensorLocalKeyFailureConsent, "FailureConsent"},
           {btkey::kCodeChallengeExpired, urnet::SnErrorCodeChallengeExpired, "SnErrorCodeChallengeExpired"},
           {btkey::kCodeSignatureMismatch, urnet::SnErrorCodeSignatureMismatch, "SnErrorCodeSignatureMismatch"},
           {btkey::kCodeNetworkOwnerOnly, urnet::SnErrorCodeNetworkOwnerOnly, "SnErrorCodeNetworkOwnerOnly"},
           {btkey::kConfirmNone, urnet::BittensorLocalKeyConfirmNone, "ConfirmNone"},
           {btkey::kConfirmDelete, urnet::BittensorLocalKeyConfirmDelete, "ConfirmDelete"},
           {btkey::kConfirmKeep, urnet::BittensorLocalKeyConfirmKeep, "ConfirmKeep"},
           {btkey::kWordWrong, urnet::BittensorLocalKeyWordWrong, "WordWrong"},
           {btkey::kReminderNone, urnet::BittensorLocalKeyReminderNone, "ReminderNone"},
           {btkey::kReminderSchedule, urnet::BittensorLocalKeyReminderSchedule, "ReminderSchedule"},
           {btkey::kReminderDeliverNow, urnet::BittensorLocalKeyReminderDeliverNow, "ReminderDeliverNow"},
           {btkey::kReminderCancel, urnet::BittensorLocalKeyReminderCancel, "ReminderCancel"},
           {btkey::kDisclaimerNone, urnet::BittensorFundsDisclaimerNone, "FundsDisclaimerNone"},
           {btkey::kDisclaimerFull, urnet::BittensorFundsDisclaimerFull, "FundsDisclaimerFull"},
           {btkey::kDisclaimerFundsOnly, urnet::BittensorFundsDisclaimerFundsOnly, "FundsDisclaimerFundsOnly"},
           {btkey::kPlaceChangeWallet, urnet::BittensorFundsDisclaimerPlaceChangeWallet, "PlaceChangeWallet"},
           {btkey::kPlaceClaimSheet, urnet::BittensorFundsDisclaimerPlaceClaimSheet, "PlaceClaimSheet"},
           {btkey::kReasonNothingPending, urnet::SnClaimReasonNothingPending, "SnClaimReasonNothingPending"},
           {btkey::kReasonUnderThreshold, urnet::SnClaimReasonUnderThreshold, "SnClaimReasonUnderThreshold"},
           {btkey::kReasonQueued, urnet::SnClaimReasonQueued, "SnClaimReasonQueued"},
           {btkey::kReasonCapReached, urnet::SnClaimReasonCapReached, "SnClaimReasonCapReached"},
           {btkey::kReasonRefused, urnet::SnClaimReasonRefused, "SnClaimReasonRefused"},
           {btkey::kReasonRelayNotRunning, urnet::SnClaimReasonRelayNotRunning, "SnClaimReasonRelayNotRunning"},
           {btkey::kReasonRelayPaused, urnet::SnClaimReasonRelayPaused, "SnClaimReasonRelayPaused"},
           {btkey::kClaimNowUnderFloor, urnet::SnClaimNowReasonUnderFloor, "SnClaimNowReasonUnderFloor"},
           {btkey::kClaimNowRequested, urnet::SnClaimNowReasonRequested, "SnClaimNowReasonRequested"},
           {btkey::kClaimNowLimit, urnet::SnClaimNowReasonLimit, "SnClaimNowReasonLimit"},
           {btkey::kActionNone, urnet::SnClaimActionNone, "SnClaimActionNone"},
       }) {
    CheckEq(pair.sdk, pair.mirrored, pair.name);
  }
  Check(urnet::BittensorLocalKeyCopySeconds == 60, "the copy's life is the sdk's 60 s");

  TEST_CASE("the card and the screen the views read are the header's own");
  urnet::BittensorLocalKeyCard card;
  card.state = std::string(btkey::kStateSaveLaterLinked);
  card.shows_key_path = false;
  card.funds_disclaimer = std::string(btkey::kDisclaimerFull);
  card.claim_funds_disclaimer = std::string(btkey::kDisclaimerNone);
  const btkey::CardLayout layout = btkey::CardLayoutFor(btkey::ConsentLine::Active, card.state, card.shows_key_path);
  Check(layout.body == btkey::CardBody::Linked, "a linked key's card");
  CheckEq("bittensor_funds_disclaimer", btkey::DisclaimerKey(card.funds_disclaimer), "its disclaimer");
  urnet::BittensorLocalKeyScreen screen;
  screen.confirming = std::string(btkey::kConfirmKeep);
  Check(btkey::KeyStepFor(screen.confirming, false) == btkey::KeyStep::Confirm, "a screen asking for words");
  card.pays_key_on_this_device = true;
  Check(card.pays_key_on_this_device, "the card says it pays a key on this device");
  urnet::SnClaimStatusResult status;
  status.shows_tile = true;
  status.shows_claimed = false;
  status.claim_tx_url = std::string("https://example.invalid/tx/0x00");
  status.claim_amount_rao = 0;
  Check(status.shows_tile && !status.shows_claimed, "the status says whether the tile and the claimed state show");
  status.reason = std::string(btkey::kReasonUnderThreshold);
  status.automatic_threshold_tao_rao = 12'500'000;
  const btkey::ReasonText reason = btkey::ReasonTextFor(status.reason, status.automatic_threshold_tao_rao,
                                                        status.automatic_threshold_rao);
  Check(reason.detail == btkey::ReasonDetail::ThresholdNoPrice, "a status with no price");
}
#endif

}  // namespace

int main() {
  TestDisclaimer();
  TestFailures();
  TestCardLayout();
  TestKeyScreen();
  TestReminder();
  TestRelay();
  TestStoreFormat();
#if defined(URNW_BITTENSOR_KEY_TESTS_SDK)
  TestAgainstSdkHeader();
#endif
  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
