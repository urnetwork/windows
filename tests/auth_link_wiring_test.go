// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// How the app uses the urnetwork://auth parser (App/AuthLink.h, run by
// auth_link_test.go) and the attempt behind it (App/UrIoApproval.h, run by
// ur_io_approval_test.go). A browser sign-in comes back as a link, and anything
// on the machine can fire one at the app, so a link may not raise the window,
// reset a screen, sign anyone in or reach the log until a sign-in this app
// started has matched it. AppController, Startup and WalletConnect need Windows
// and WinRT, so these read their sources with every comment blanked. The pins on
// the gate are allow-lists: the text of the functions that decide is the whole
// text that may be there, so a statement added to one of them is a change to this
// file made on purpose, not one that slips past a list of what is forbidden.

var (
	authLinkSourcesOnce  sync.Once
	authLinkSourcesCache map[string]string
)

// The sources of app/src (the app, the shared headers, the service and the
// updater) with every comment blanked, keyed by their slash-separated path under
// app/src. Build output is not source. Read once for all the tests here.
func authLinkSourceFiles(t *testing.T) map[string]string {
	t.Helper()
	authLinkSourcesOnce.Do(func() { authLinkSourcesCache = readAuthLinkSourceFiles(t) })
	if authLinkSourcesCache == nil {
		t.Fatal("the sources of app/src could not be read")
	}
	return authLinkSourcesCache
}

func readAuthLinkSourceFiles(t *testing.T) map[string]string {
	t.Helper()
	root := filepath.Join(repositoryRoot(t), "app", "src")
	skipped := map[string]bool{"Generated Files": true, "x64": true, "ARM64": true, "bin": true, "obj": true}
	files := map[string]string{}
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != root && skipped[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if ext := strings.ToLower(filepath.Ext(path)); ext != ".cpp" && ext != ".h" {
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		files[filepath.ToSlash(relative)] = stripComments(lf(string(data)))
		return nil
	})
	if err != nil {
		t.Error(err)
		return nil
	}
	return files
}

// The 1-based line of offset `at` in code.
func authLinkLine(code string, at int) int { return strings.Count(code[:at], "\n") + 1 }

// Code made comparable: every run of whitespace is one space.
func authLinkSquash(code string) string { return strings.Join(strings.Fields(code), " ") }

// Require the definition of `signature` in `source` to be exactly `want`, to the
// whitespace: the allow-list of what a function that decides may do.
func requireExactDefinition(t *testing.T, name, source, signature, want string) {
	t.Helper()
	got := authLinkSquash(definitionBody(t, name, source, signature))
	if expected := authLinkSquash(want); got != expected {
		t.Errorf("%s does not match the text this contract allows (comments and whitespace aside).\n got: %s\nwant: %s\n"+
			"A statement added, removed or turned round here can raise the window, show a screen or sign someone in for a link nothing asked for: "+
			"change this contract on purpose, with the review that deserves.", signature, got, expected)
	}
}

// An auth link is judged before anything is raised. HandleDeepLink logs that a
// link arrived, sends the auth host to HandleAuthLink and returns, and only then
// brings the window forward for every other host. HandleAuthLink checks that the
// app is not quitting, parses the link, asks the attempt, opens the box of an
// approval with the attempt's own key, logs closed tokens and then, by verdict:
// ignores a link when signed in; raises nothing for a stray (but the first launch
// of a fresh process, ShowStrayNotice); raises the window for the live attempt's
// denial; and for its approval consumes the attempt, raises the window and hands
// the code on, in that order. Nothing of the stateless path that signed anyone in
// with a code out of any link is left.
func TestAuthLinkIsGatedBeforeTheWindowIsRaised(t *testing.T) {
	source := stripComments(lf(readAppSource(t, "AppController.cpp")))

	handle := definitionBody(t, "AppController.cpp", source, "void AppController::HandleDeepLink(")
	body := strings.TrimSpace(handle[strings.Index(handle, "{")+1:])
	if !strings.HasPrefix(body, `LogInfo("app: deep link received");`) {
		t.Error(`HandleDeepLink must start with LogInfo("app: deep link received"): the live-desktop probes key on that line, and nothing may run before it`)
	}
	signOutRequireInOrder(t, "AppController::HandleDeepLink", handle,
		regexp.QuoteMeta(`LogInfo("app: deep link received");`),
		`if \(authlink::IsAuthHost\(url\)\) \{\s*HandleAuthLink\(url\);\s*return;\s*\}`,
		regexp.QuoteMeta("ShowWindow(nullptr);"),
		regexp.QuoteMeta(`url.rfind("urnetwork://onboarding/", 0) == 0`),
		`try \{\s*sdk_\.HandleDeepLink\(url\);\s*\} catch \(const std::exception&\)`)
	if count := strings.Count(handle, "ShowWindow("); count != 1 {
		t.Errorf("HandleDeepLink raises the window %d times; once, for the hosts that are not auth, after the auth host has been judged", count)
	}

	// the gate, in the order that matters
	gate := definitionBody(t, "AppController.cpp", source, "void AppController::HandleAuthLink(")
	signOutRequireInOrder(t, "AppController::HandleAuthLink", gate,
		regexp.QuoteMeta("if (quitting_.load(std::memory_order_acquire)) return;"),
		regexp.QuoteMeta("authlink::Parse(url)"),
		regexp.QuoteMeta("approval_.Check(link, approval::Now(), ApprovalContext())"),
		regexp.QuoteMeta("approval::Unseal(approval_.SecretKey(), link)"),
		regexp.QuoteMeta(`LogInfo("app: auth link: {} ({})", approval::ToString(decision.verdict), why);`),
		`switch \(decision\.verdict\) \{`,
		`case approval::Verdict::Ignored:\s*return;`,
		`case approval::Verdict::Stray:\s*if \(decision\.why == approval::Why::Expired \|\| decision\.why == approval::Why::Superseded\) \{\s*if \(LoginPage\* login = LoginPageOrNull\(\)\) login->OnApprovalExpired\(\);\s*\}\s*ShowStrayNotice\(\);\s*return;`,
		`case approval::Verdict::Denied:\s*ShowWindow\(nullptr\);`,
		regexp.QuoteMeta("login->OnApprovalDenied();"),
		`case approval::Verdict::Accepted:\s*if \(!approval_\.Consume\(link\.state\)\) return;\s*ShowWindow\(nullptr\);`,
		regexp.QuoteMeta("login->OnApprovedCode(std::move(*code));"))
	if strings.Contains(gate, "default:") {
		t.Error("HandleAuthLink's switch has a default: a verdict added to the model must be a build error until the gate has decided what it does")
	}
	// a stray and an ignored link raise nothing: the cases up to Denied hold no ShowWindow(
	if beforeDenied := gate[:strings.Index(gate, "case approval::Verdict::Denied:")]; strings.Contains(beforeDenied, "ShowWindow(") {
		t.Error("HandleAuthLink raises the window for a link that is ignored or a stray")
	}
	if count := strings.Count(gate, "ShowWindow("); count != 2 {
		t.Errorf("HandleAuthLink raises the window %d times; the live attempt's denial and its approval may, and nothing else", count)
	}
	// the build refuses a switch that misses a verdict
	provideRequire(t, "AppController.cpp", lf(readAppSource(t, "AppController.cpp")),
		"#pragma warning(push)\n#pragma warning(error : 4062)\nvoid AppController::HandleAuthLink(", "#pragma warning(pop)")

	// ... and this is the whole of it
	requireExactDefinition(t, "AppController.cpp", source, "void AppController::HandleAuthLink(", `
void AppController::HandleAuthLink(const std::string& url) {
  if (quitting_.load(std::memory_order_acquire)) return;
  const authlink::Return link = authlink::Parse(url);
  approval::Decision decision = approval_.Check(link, approval::Now(), ApprovalContext());
  std::optional<std::string> code;
  if (decision.verdict == approval::Verdict::Accepted) {
    code = approval::Unseal(approval_.SecretKey(), link);
    if (!code) decision = approval::Decision{approval::Verdict::Stray, approval::Why::BadSeal};
  }
  const std::string_view why = AuthLinkWhy(link, decision);
  LogInfo("app: auth link: {} ({})", approval::ToString(decision.verdict), why);
  switch (decision.verdict) {
    case approval::Verdict::Ignored:
      return;
    case approval::Verdict::Stray:
      if (decision.why == approval::Why::Expired || decision.why == approval::Why::Superseded) {
        if (LoginPage* login = LoginPageOrNull()) login->OnApprovalExpired();
      }
      ShowStrayNotice();
      return;
    case approval::Verdict::Denied:
      ShowWindow(nullptr);
      if (LoginPage* login = LoginPageOrNull()) login->OnApprovalDenied();
      return;
    case approval::Verdict::Accepted:
      if (!approval_.Consume(link.state)) return;
      ShowWindow(nullptr);
      if (LoginPage* login = LoginPageOrNull()) login->OnApprovedCode(std::move(*code));
      return;
  }
}`)
	// what a stray may do: the first launch of a fresh process shows the window once with the neutral line
	requireExactDefinition(t, "AppController.cpp", source, "void AppController::ShowStrayNotice(", `
void AppController::ShowStrayNotice() {
  if (!freshProcessLaunch_) return;
  freshProcessLaunch_ = false;
  if (!neutralLine_.Allow(std::chrono::steady_clock::now())) return;
  ShowWindow(nullptr);
  if (LoginPage* login = LoginPageOrNull()) login->ShowStrayNotice();
}`)
	// what the gate asks of the app: signed in, and the flow
	requireExactDefinition(t, "AppController.cpp", source, "approval::Context AppController::ApprovalContext(", `
approval::Context AppController::ApprovalContext() {
  approval::Context context;
  context.signedIn = authState_ == AuthState::LoggedIn || sdk_.IsLoggedIn();
  context.flow = sdk_.CurrentBrowserFlow();
  return context;
}`)
	// the signed-in test is the signed-in state: Authenticating sticks after an abandoned attempt
	// and a gate that waited for it to clear would be closed for good
	if strings.Contains(definitionBody(t, "AppController.cpp", source, "approval::Context AppController::ApprovalContext("), "Authenticating") {
		t.Error("the gate asks whether the app is signed in, not whether a sign-in is under way: Authenticating sticks after an abandoned Google, Apple or wallet attempt")
	}

	// nothing of the stateless path is left in the controller, and the link is read by the parser alone
	for _, gone := range []string{"IsAuthDeepLink(", "DeepLinkQueryParam(", "SignInWithAuthCode(", "JudgeAuthLink(", "AuthVerdict"} {
		if strings.Contains(source, gone) {
			t.Errorf("AppController.cpp still has %s: the old path read a code out of any auth link and signed in with it", gone)
		}
	}
	if count := strings.Count(source, "authlink::Parse("); count != 1 {
		t.Errorf("AppController.cpp parses an auth link %d times; HandleAuthLink does it once, and nothing else reads one", count)
	}
	// the callers of the gate: the router, and nothing else
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "HandleAuthLink(") && name != "App/AppController.cpp" && name != "App/AppController.h" {
			t.Errorf("%s names HandleAuthLink: only the router (HandleDeepLink) reaches the gate", name)
		}
	}
	if count := strings.Count(source, "HandleAuthLink("); count != 2 { // the call and the definition
		t.Errorf("AppController.cpp names HandleAuthLink %d times; HandleDeepLink calls it once and it is defined once", count)
	}
	// and the router is reached only through the launch the instance serves
	if count := strings.Count(source, "HandleDeepLink("); count != 3 { // ServeLaunch's call, the definition, sdk_.HandleDeepLink
		t.Errorf("AppController.cpp names HandleDeepLink %d times; ServeLaunch calls it once, it is defined once, and it hands the other hosts to the sdk", count)
	}
}

// The first launch of a freshly started process is the one launch whose stray auth link may show
// the window (once, with the neutral line): the flag that says so is set where launches are
// served, from "has any launch been served", never in App::OnLaunched, whose shape is pinned,
// and it is read in one place.
func TestAuthLinkStrayShowsTheWindowOnlyForTheFirstLaunchOfAProcess(t *testing.T) {
	controller := stripComments(lf(readAppSource(t, "AppController.cpp")))
	serve := definitionBody(t, "AppController.cpp", controller, "void AppController::ServeLaunch(const instance::LaunchRequest& request) {")
	signOutRequireInOrder(t, "AppController::ServeLaunch", serve,
		regexp.QuoteMeta("const instance::LaunchAction action = instance::ActionFor(request);"),
		regexp.QuoteMeta("freshProcessLaunch_ = !launchServed_;"),
		regexp.QuoteMeta("launchServed_ = true;"),
		regexp.QuoteMeta("switch (action) {"))
	if count := strings.Count(controller, "freshProcessLaunch_ = "); count != 2 {
		t.Errorf("freshProcessLaunch_ is assigned %d times in AppController.cpp; ServeLaunch sets it and ShowStrayNotice clears it", count)
	}
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "freshProcessLaunch_") && name != "App/AppController.cpp" && name != "App/AppController.h" {
			t.Errorf("%s reads the first-launch flag: only AppController decides what a stray earns", name)
		}
	}
	app := stripComments(lf(readAppSource(t, "App.xaml.cpp")))
	quitForbid(t, "App.xaml.cpp", app, "App::OnLaunched is pinned to serve every launch through ServeLaunch's one rule",
		"freshProcessLaunch_", "launchServed_", "ShowStrayNotice", "HandleAuthLink")
	if count := strings.Count(controller, "freshProcessLaunch_)"); count != 1 {
		t.Errorf("AppController.cpp tests the first-launch flag %d times; ShowStrayNotice alone does", count)
	}
	// the login page's side of it is a quiet, non-modal line, never the red error line or a screen change
	login := stripComments(lf(readAppSource(t, "LoginPage.cpp")))
	stray := definitionBody(t, "LoginPage.cpp", login, "void LoginPage::ShowStrayNotice() {")
	quitForbid(t, "LoginPage::ShowStrayNotice", stray,
		"a stray changes no screen and clobbers no error", "SetInitialLoginError(", "ShowLoginStep(", "ResetToInitialStep(",
		"SignInWithAuthCode(", "LoginWithCode(", "ShowErrorOnCurrentStep(", "Sdk()")
}

// What the sdk throws, and what a link may bring it to throw, is caught where the link arrived. HandleDeepLink
// hands every host but auth to the sdk, whose crypto throws on a malformed wallet link; nothing may leave
// the handler, and the log gets a message that holds neither the link nor what the exception says.
func TestDeepLinkDropsALinkTheSdkCannotHandle(t *testing.T) {
	source := stripComments(lf(readAppSource(t, "AppController.cpp")))
	handle := definitionBody(t, "AppController.cpp", source, "void AppController::HandleDeepLink(")
	caught := regexp.MustCompile(`try \{\s*sdk_\.HandleDeepLink\(url\);\s*\} catch \(const std::exception&\) \{([^{}]*)\}`).FindStringSubmatch(handle)
	if caught == nil {
		t.Fatal("HandleDeepLink must call sdk_.HandleDeepLink(url) inside try { } catch (const std::exception&) { }: the wallet bridge's crypto throws on a malformed link, and an exception that leaves the handler ends the app")
	}
	if leak := regexp.MustCompile(`\burl\b|\bwhat\s*\(|\bNarrow\s*\(|\be\b`).FindString(caught[1]); leak != "" {
		t.Errorf("the catch in HandleDeepLink uses %q: the log gets a closed message, because a link is a secret and what an exception says can quote it", leak)
	}
	if !strings.Contains(caught[1], "LogWarn(") {
		t.Error("the catch in HandleDeepLink drops the link without a line in the log")
	}
}

// A one-time code is signed in with only where the person put it: the auth-code sheet and the
// browser sign-in sheet's field. LoginPage::SignInWithAuthCode resets the screen to the initial
// step before anything else, and a link nobody asked for must not do that. The approval's code is
// redeemed only by the sheet that shows the person which network they would join, and the
// controller never redeems: it opens the box and hands the code on.
func TestAuthLinkCodeLoginIsOnlyReachedFromTheSheets(t *testing.T) {
	sheets := map[string]bool{"App/LoginPage.cpp": true, "App/LoginPage.h": true, "App/BrowserApprovalSheet.cpp": true, "App/BrowserApprovalSheet.h": true}
	host := map[string]bool{"App/SdkHost.cpp": true, "App/SdkHost.h": true}
	for name, code := range authLinkSourceFiles(t) {
		if !sheets[name] && strings.Contains(code, "SignInWithAuthCode(") {
			t.Errorf("%s calls SignInWithAuthCode: only the login page's sheets do, with a code the person entered", name)
		}
		if !sheets[name] && !host[name] && strings.Contains(code, "LoginWithCode(") {
			t.Errorf("%s calls LoginWithCode: only the login page signs in with a code, and only one the person entered", name)
		}
		for _, call := range []string{"RedeemApprovedCode(", "ConfirmApprovedNetwork(", "DiscardApprovedNetwork("} {
			if !sheets[name] && !host[name] && strings.Contains(code, call) {
				t.Errorf("%s calls %s: the sheet that shows the person which network they would join redeems and confirms; the controller only hands the code on", name, call)
			}
		}
		for _, hook := range []string{"OnApprovedCode(", "OnApprovalDenied(", "OnApprovalExpired("} {
			if name != "App/AppController.cpp" && !sheets[name] && strings.Contains(code, hook) {
				t.Errorf("%s calls %s: only AppController's gate and timer do", name, hook)
			}
		}
	}
	login := stripComments(lf(readAppSource(t, "LoginPage.cpp")))
	if count := strings.Count(login, "SignInWithAuthCode("); count != 3 {
		t.Errorf("LoginPage.cpp has %d mentions of SignInWithAuthCode(; want the definition and the two sheets' calls", count)
	}
	// the auth-code sheet signs in with the code in its own field; the browser sign-in sheet hands the
	// code its password box held to the page that hosts it, which signs in once the sheet is gone
	for sheet, call := range map[string]string{
		"LoginPage::OnUseCode(":         "SignInWithAuthCode(urnw::Narrow(field.Text().c_str()));",
		"LoginPage::ShowApprovalSheet(": "SignInWithAuthCode(std::move(pasted));",
	} {
		if !strings.Contains(functionBody(login, sheet), call) {
			t.Errorf("%s no longer signs in with the code the person put in its own field (%s)", sheet, call)
		}
	}
	hosting := definitionBody(t, "LoginPage.cpp", login, "winrt::fire_and_forget LoginPage::ShowApprovalSheet(")
	signOutRequireInOrder(t, "LoginPage::ShowApprovalSheet", hosting,
		regexp.QuoteMeta("pasted = approvalSheet_->TakePastedCode();"),
		regexp.QuoteMeta("w_.SetSheetOpen(false);"),
		regexp.QuoteMeta("SignInWithAuthCode(std::move(pasted));"))
	if count := strings.Count(login, "LoginWithCode("); count != 1 {
		t.Errorf("LoginPage.cpp calls LoginWithCode %d times; SignInWithAuthCode does, once", count)
	}
}

// The text the raw command line may be handed to. A sign-in return rides on the
// command line of the process the browser starts, and anything else done with it
// can end in a log line.
var authLinkCommandLineConsumers = map[string]bool{
	"HasArgument":             true, // looks for a flag and keeps nothing
	"DeepLinkFromCommandLine": true, // cuts the link out; its result is never logged
	"CommandLineToArgvW":      true, // splits it for the flags
	"RedactCommandLine":       true, // authlink::RedactCommandLine: what the log gets
}

var authLinkCommandLineSource = regexp.MustCompile(`(?:::)?GetCommandLine[AW]?\s*\(\s*\)|\b[A-Za-z_]\w*\.Arguments\s*\(\s*\)`)

// The places in code where the process's command line, or an activation's
// arguments, goes anywhere but to one of the consumers above.
func authLinkRawCommandLineUses(code string) []string {
	var problems []string
	for _, location := range authLinkCommandLineSource.FindAllStringIndex(code, -1) {
		before := strings.TrimRight(code[:location[0]], " \t\r\n")
		callee := ""
		if strings.HasSuffix(before, "(") {
			if match := regexp.MustCompile(`([A-Za-z_]\w*)\s*$`).FindStringSubmatch(before[:len(before)-1]); match != nil {
				callee = match[1]
			}
		}
		if !authLinkCommandLineConsumers[callee] {
			problems = append(problems, "line "+strconv.Itoa(authLinkLine(code, location[0]))+": "+
				strings.TrimSpace(code[location[0]:location[1]])+" goes to "+strconv.Quote(callee))
		}
	}
	return problems
}

// The process's command line never reaches a log: a protocol launch puts the
// whole link on it, and the startup diagnostics (urnetwork-app.log, and
// --diagnose) used to print it as it was. The diagnostics line prints
// authlink::RedactCommandLine of it, and nothing else in the sources hands the
// raw line, or an activation's arguments, to anything that could log it.
func TestCommandLineIsNeverLoggedRaw(t *testing.T) {
	for name, code := range authLinkSourceFiles(t) {
		for _, problem := range authLinkRawCommandLineUses(code) {
			t.Errorf("%s %s: the raw command line may go only to %s", name, problem,
				"HasArgument, DeepLinkFromCommandLine, CommandLineToArgvW or RedactCommandLine")
		}
	}

	diagnostics := authLinkSourceFiles(t)["App/Startup.cpp"]
	collect := definitionBody(t, "Startup.cpp", diagnostics, "std::vector<std::wstring> CollectDiagnostics() {")
	provideRequire(t, "CollectDiagnostics", collect,
		`std::format(L"  command line     : {}",`, "authlink::RedactCommandLine(::GetCommandLineW())")

	// the router cuts the link out with the parser's own finder, which ignores the case of the scheme
	controller := authLinkSourceFiles(t)["App/AppController.cpp"]
	provideRequire(t, "DeepLinkFromCommandLine", definitionBody(t, "AppController.cpp", controller, "std::wstring DeepLinkFromCommandLine(std::wstring_view commandLine) {"),
		"return std::wstring(authlink::LinkInCommandLine(commandLine));")

	// the check itself: it flags the line that was there, and passes the ones that are
	for _, bad := range []string{
		`lines.push_back(std::format(L"  command line     : {}", ::GetCommandLineW()));`,
		`const std::wstring line = ::GetCommandLineW();`,
		`LogInfo("launched with {}", Narrow(launchArgs.Arguments()));`,
	} {
		if len(authLinkRawCommandLineUses(bad)) != 1 {
			t.Errorf("the check does not flag %q", bad)
		}
	}
	for _, good := range []string{
		`lines.push_back(std::format(L"{}", authlink::RedactCommandLine(::GetCommandLineW())));`,
		`return instance::HasArgument(::GetCommandLineW(), instance::kAutostartArgument);`,
		`wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);`,
		`return Narrow(DeepLinkFromCommandLine(launchArgs.Arguments()));`,
	} {
		if problems := authLinkRawCommandLineUses(good); len(problems) != 0 {
			t.Errorf("the check flags %q: %v", good, problems)
		}
	}
}

// What a log call, a debugger line or the wallet bridge's error callbacks may not be given.
// A value that came from a link is a secret until the sign-in it answers consumes it, and the
// url, the Return, a key of an attempt and the link's query identify one. The Solana error
// callback is given the bridge page's own English (pageText) next to the message the person
// sees: SdkHost logs it as one bounded line only when a Solana flow takes the answer, as the
// sso callback logs its error once it matched (solana_wallet_test.go).
var authLinkLogSink = regexp.MustCompile(`\b(?:Log(?:Info|Warn|Error|Debug)|OutputDebugString[AW]?|on_error|on_solana_error)\s*\(`)

// Names that hold a link, or a value taken from one, wherever they stand alone.
var authLinkSecretNames = map[string]bool{
	"url": true, "uri": true, "deepLink": true, "link": true, "code": true, "state": true, "data": true,
	"epk": true, "nonce": true, "publicKey": true, "secretKey": true, "byJwt": true, "authCode": true,
}

// Names that hold a link's text only in the file of the host they belong to: the query of a wallet
// bridge's return. (SdkHost has a `query` of its own, a search of locations.)
var authLinkFileSecretNames = map[string][]string{
	"App/WalletConnect.cpp": {"query", "params"},
}

// A member of an object that holds a link, a key of an attempt or a launch request. The member of
// anything else (a tunnel's `status.state`) is not what this guards.
var authLinkSecretMember = regexp.MustCompile(
	`\b(?:link|request|view|keys|params|pair|answer|attempt|opened|launch)\s*(?:\.|->)\s*` +
		`(?:state|code|data|epk|nonce|publicKey|secretKey|deepLink)\b`)

// The arguments of the call whose '(' is at open, split at the commas of its own
// level; string and character literals are one piece.
func authLinkCallArguments(code string, open int) []string {
	var arguments []string
	depth, start := 0, open+1
	for at := open; at < len(code); at++ {
		switch character := code[at]; {
		case character == '"' || character == '\'':
			for at++; at < len(code) && code[at] != character && code[at] != '\n'; at++ {
				if code[at] == '\\' {
					at++
				}
			}
		case character == '(' || character == '[' || character == '{':
			depth++
		case character == ')' || character == ']' || character == '}':
			depth--
			if depth == 0 {
				return append(arguments, code[start:at])
			}
		case character == ',' && depth == 1:
			arguments = append(arguments, code[start:at])
			start = at + 1
		}
	}
	return append(arguments, code[start:])
}

var (
	authLinkStringLiteral = regexp.MustCompile(`L?"(?:\\.|[^"\\\n])*"`)
	authLinkIdentifier    = regexp.MustCompile(`[A-Za-z_]\w*`)
)

// The secrets in one argument (its string literals blanked): a link-bearing name standing alone, or
// a member that is one of a link-bearing object. A name that is called (e.code()) is a function,
// a name or a member after '.', '->' or '::' belongs to something else, and the length of a secret
// (url.size()) is not the secret.
func authLinkArgumentLeaks(argument string, extra ...string) []string {
	var leaks []string
	secretNames := authLinkSecretNames
	if len(extra) > 0 {
		secretNames = map[string]bool{}
		for name := range authLinkSecretNames {
			secretNames[name] = true
		}
		for _, name := range extra {
			secretNames[name] = true
		}
	}
	for _, member := range authLinkSecretMember.FindAllString(argument, -1) {
		leaks = append(leaks, strings.Join(strings.Fields(member), ""))
	}
	for _, location := range authLinkIdentifier.FindAllStringIndex(argument, -1) {
		name := argument[location[0]:location[1]]
		if !secretNames[name] {
			continue
		}
		before := strings.TrimRight(argument[:location[0]], " \t\r\n")
		if strings.HasSuffix(before, ".") || strings.HasSuffix(before, "->") || strings.HasSuffix(before, "::") {
			continue
		}
		after := strings.TrimLeft(argument[location[1]:], " \t\r\n")
		if strings.HasPrefix(after, "(") {
			continue
		}
		if member := regexp.MustCompile(`^(?:\.|->)\s*(\w+)`).FindStringSubmatch(after); member != nil {
			switch member[1] {
			case "size", "length", "empty":
				continue
			}
		}
		leaks = append(leaks, name)
	}
	return leaks
}

// The sinks in code that are given a secret: after the format string for a log call, in any
// argument for the others.
func authLinkLogLeaks(code string, extra ...string) []string {
	var leaks []string
	for _, location := range authLinkLogSink.FindAllStringIndex(code, -1) {
		arguments := authLinkCallArguments(code, location[1]-1)
		if strings.HasPrefix(code[location[0]:], "Log") && len(arguments) > 0 {
			arguments = arguments[1:]
		}
		for _, argument := range arguments {
			for _, leak := range authLinkArgumentLeaks(authLinkStringLiteral.ReplaceAllString(argument, `""`), extra...) {
				leaks = append(leaks, "line "+strconv.Itoa(authLinkLine(code, location[0]))+" passes "+leak)
			}
		}
	}
	return leaks
}

// The files that receive, route, hand over or open a link, or hold a secret of an attempt, log
// only closed words: the controller, the sdk host, the registrar, the page check, the login page
// and the browser sign-in sheet.
func TestNoLogCallReceivesALinkOrASecret(t *testing.T) {
	files := authLinkSourceFiles(t)
	checked := 0
	for _, name := range []string{
		"App/AppController.cpp", "App/main.cpp", "App/SingleInstance.cpp", "App/Startup.cpp", "App/WalletConnect.cpp",
		"App/LoginPage.cpp", "App/SdkHost.cpp", "App/ProtocolHandler.cpp", "App/ApprovalCheck.cpp",
		"App/BrowserApprovalSheet.cpp",
	} {
		code, ok := files[name]
		if !ok {
			t.Errorf("%s was not found: it is one of the files that must log closed words only", name)
			continue
		}
		checked++
		for _, leak := range authLinkLogLeaks(code, authLinkFileSecretNames[name]...) {
			t.Errorf("%s %s: a link and the values in it are secrets; log a closed word instead (authlink::ToString, a verdict's token)", name, leak)
		}
	}
	if checked < 10 {
		t.Fatalf("only %d of the link-handling sources were found; update this contract", checked)
	}

	// the check itself
	for _, bad := range []string{
		`LogInfo("app: deep link {}", url);`,
		"LogWarn(\"app: {}\",\n         request.deepLink);",
		`LogInfo("x {} {}", ToString(verdict), link.state);`,
		`urnw::LogError("x {}", Narrow(code));`,
		`LogDebug("sealed {}", view.publicKey);`,
		`LogWarn("keys {}", keys->secretKey);`,
		`OutputDebugStringA(url.c_str());`,
		`on_error(err + url);`,
	} {
		if len(authLinkLogLeaks(bad)) == 0 {
			t.Errorf("the check does not flag %q", bad)
		}
	}
	for _, bad := range []string{
		`LogInfo("q {}", Narrow(query));`,
		`if (on_error) on_error("failed: " + query);`,
		`LogWarn("p {}", params["data"]);`,
		// the page's code or text reaches neither error callback: the sdk reads the code, and
		// the text goes only to the Solana callback, held in a name of its own
		`on_error(LocalizedBridgeError(p, params["errorCode"], pageText));`,
		`on_solana_error(std::move(message), params["errorMessage"]);`,
	} {
		if len(authLinkLogLeaks(bad, authLinkFileSecretNames["App/WalletConnect.cpp"]...)) == 0 {
			t.Errorf("the check does not flag %q in the wallet bridge's file", bad)
		}
	}
	if len(authLinkLogLeaks(`LogWarn("search {}", query);`)) != 0 {
		t.Error("the check flags a `query` in a file whose query is not a link's")
	}
	for _, good := range []string{
		`LogInfo("app: auth link: {} ({})", approval::ToString(decision.verdict), why);`,
		`LogInfo("the code and the url are never logged");`,
		`LogError("x: {} (0x{:08x})", Narrow(std::wstring{e.message()}), static_cast<uint32_t>(e.code()));`,
		`LogWarn("startup: {}", Narrow(line));`,
		`LogInfo("tunnel {}", status.state);`,
		`LogInfo("a {} b {}", data.size(), url.length());`,
		`LogInfo("app: launch{}: {}", request.autostart ? " (autostart)" : "", instance::ToString(action));`,
		`on_error("failed to derive shared secret");`,
		`on_solana_error(std::move(message), pageText);`,
		`LogInfo("approval: the attempt ended: {}", approval::ToString(reason));`,
	} {
		if leaks := authLinkLogLeaks(good); len(leaks) != 0 {
			t.Errorf("the check flags %q: %v", good, leaks)
		}
	}
}

// Where a percent-decoder or a query parser may be, and why. The values of an
// auth link are read by authlink::Parse and nothing else: UrlQuery.h's decoder
// keeps malformed escapes, turns '+' into a space and lets the last duplicate
// win, and the hand-rolled one in AppController.cpp that this replaced took the
// first duplicate, decoded %00 and read any key. Each of these decodes the links
// of its own host.
var authLinkDecoders = map[string]string{
	"App/UrlQuery.h":          "the shared decoder of the wallet bridges and the pay hand-back",
	"App/WalletConnect.cpp":   "the wallet bridges' returns",
	"App/BalanceSheets.cpp":   "the pay hand-back",
	"App/MainWindow.xaml.cpp": "the campaign emails' links (HandleOnboardingLink)",
}

var authLinkDecoderUse = regexp.MustCompile(`\bhexv\b|\bPercentDecode\s*\(|\bParseQueryString\s*\(|\bParseUrlQuery\s*\(|WwwFormUrlDecoder|\bQueryParsed\b|\bUrlUnescape`)

func TestNoSourceDecodesDeepLinkValuesOutsideAuthLink(t *testing.T) {
	files := authLinkSourceFiles(t)
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		code := files[name]
		if _, allowed := authLinkDecoders[name]; !allowed && authLinkDecoderUse.MatchString(code) {
			t.Errorf("%s decodes link values itself (%s): read an auth link only through authlink::Parse, and add a file here only for the links of its own host",
				name, authLinkDecoderUse.FindString(code))
		}
		if name != "App/AuthLink.h" && strings.Contains(code, "urnetwork://auth") {
			t.Errorf("%s names the auth host: only AuthLink.h (IsAuthHost, kApprovalV1) knows what the auth host is", name)
		}
	}
	controller := files["App/AppController.cpp"]
	if authLinkDecoderUse.MatchString(controller) || strings.Contains(controller, "'%'") {
		t.Error("AppController.cpp decodes link values: the router hands an auth link to authlink::Parse and reads nothing out of it")
	}
	// nor does the parser: it has no decoder
	if parser := files["App/AuthLink.h"]; authLinkDecoderUse.MatchString(parser) {
		t.Error("AuthLink.h decodes link values: a percent sign is refused, never read")
	}

	// the check itself: it flags the decoder this replaced
	removed := `std::string DeepLinkQueryParam(const std::string& url, std::string_view name) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    return -1;
  };
  if (value[k] == '%' && k + 2 < value.size()) {}
}`
	if !authLinkDecoderUse.MatchString(removed) {
		t.Error("the check does not flag the hand-rolled decoder that AppController.cpp used to have")
	}
}

// The parser is a header of the App project, and the project lists the headers
// it owns.
func TestAuthLinkHeaderIsListedInTheProject(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="AuthLink.h" />`) {
		t.Error("App.vcxproj does not list AuthLink.h")
	}
}

// Exporting the log copies it, and a build before the redaction wrote the whole command line of
// every protocol launch to it, which carries a sign-in return (an Apple identity token, a code).
// The copy is made line by line through authlink::RedactLogText (LogExport.h, run by log_export_test.go
// against real files), and the log itself is not touched: nothing in the export opens it for writing,
// removes it or copies it as it is, and saving over the log itself is refused before anything is opened
// for writing (the picker suggests the log's own name, and a file opened to be written is emptied first).
func TestExportedLogsHaveNoLinks(t *testing.T) {
	settings := stripComments(lf(readAppSource(t, "SettingsPage.cpp")))
	save := definitionBody(t, "SettingsPage.cpp", settings, "winrt::fire_and_forget SettingsPage::SaveLogsToFile() {")
	signOutRequireInOrder(t, "SettingsPage::SaveLogsToFile", save,
		regexp.QuoteMeta("PickSaveFileAsync()"),
		regexp.QuoteMeta("if (!logexport::CopyWithoutLinks(source, std::filesystem::path{std::wstring{file.Path()}})) {"),
		regexp.QuoteMeta(`throw std::runtime_error("the log could not be copied");`),
		regexp.QuoteMeta(`snackbar_.Show(Loc("save_logs"), InfoBarSeverity::Success);`))
	quitForbid(t, "SettingsPage::SaveLogsToFile", save, "the exported log is the redacted copy, and a copy that was refused is not reported as saved",
		"copy_file(", "std::ofstream", "std::ifstream")
	provideRequire(t, "SettingsPage.cpp", settings, `#include "LogExport.h"`)
	// the function that copies lives in the header, where its spec can run it; it is not written a second time here
	if strings.Contains(settings, "CopyLogWithoutLinks") || strings.Contains(settings, "std::ofstream") {
		t.Error("SettingsPage.cpp copies the log itself: the copy is logexport::CopyWithoutLinks (LogExport.h), whose spec writes real files")
	}
	// the log is exported nowhere else as it is
	for name, code := range authLinkSourceFiles(t) {
		if name != "App/SettingsPage.cpp" && strings.Contains(code, "LogFilePath()") && strings.Contains(code, "copy_file(") {
			t.Errorf("%s copies the log file: an export goes through logexport::CopyWithoutLinks", name)
		}
		if name != "App/SettingsPage.cpp" && name != "App/LogExport.h" && strings.Contains(code, "CopyWithoutLinks(") {
			t.Errorf("%s calls CopyWithoutLinks: Export logs (SettingsPage) is the one export", name)
		}
	}
}

// The wallet bridge's returns carry the wallet's key, a nonce and a box, and the approval's answer
// carries the page's, all off a link that any program can fire at the app. The SDK's crypto throws
// (urnet::Error) for a key or a nonce of the wrong length and for a failed decryption, and the
// handlers run on the UI thread under ServeLaunch, which catches nothing: every call sits in a try
// that catches std::exception, so a malformed link is an error message and not the end of the app.
func TestAuthLinkWalletBridgeFailuresStayInsideTheHandler(t *testing.T) {
	files := authLinkSourceFiles(t)
	// the two files that call them have to be found calling them; no other file may call one unguarded
	mustCall := map[string]bool{"App/WalletConnect.cpp": true, "App/ApprovalUnseal.h": true}
	for name, code := range files {
		if problems := authLinkUnguardedThrowers(t, code, mustCall[name]); len(problems) != 0 {
			t.Errorf("%s: %s", name, strings.Join(problems, "; "))
		}
	}
	// the gate itself never opens a box: it asks the one function that does
	if controller := files["App/AppController.cpp"]; strings.Contains(controller, "urnet::decryptData(") || strings.Contains(controller, "urnet::generateSharedSecret(") {
		t.Error("AppController.cpp opens a box itself: approval::Unseal (ApprovalUnseal.h) is the one place that does")
	}
	// the wallet bridges decrypt through TryDecrypt, and a decryption that failed (an empty answer, or none) is an error and not
	// a document: without the check the handler reads the beginning of a vector that is not there, which is the crash a
	// malformed link used to be able to cause. (TryDecrypt and SharedSecretBase58 are pinned whole in
	// ur_io_approval_exact_test.go.)
	wallet := files["App/WalletConnect.cpp"]
	for handler, failure := range map[string]string{
		"void WalletConnect::HandleConnect(":     "failed to decrypt wallet connection",
		"void WalletConnect::HandleSignMessage(": "failed to decrypt wallet signature",
	} {
		body := definitionBody(t, "WalletConnect.cpp", wallet, handler)
		guard := `const auto decrypted = TryDecrypt\(params\["data"\], params\["nonce"\], \*sharedB58\);\s*` +
			`if \(!decrypted \|\| decrypted->empty\(\)\) \{\s*FailSolana\(p, urnet::SolanaWalletFailureSourceApp, urnet::SolanaWalletFailureWallet,\s*"` +
			regexp.QuoteMeta(failure) + `"\);\s*return;\s*\}`
		signOutRequireInOrder(t, handler, body, guard, regexp.QuoteMeta("decrypted->begin()"))
		if strings.Contains(body, "urnet::decryptData(") {
			t.Errorf("%s decrypts without TryDecrypt, which catches what the SDK throws", handler)
		}
	}
	if !strings.Contains(definitionBody(t, "WalletConnect.cpp", wallet, "std::optional<std::string> WalletConnect::SharedSecretBase58() const {"), "catch (const std::exception&)") {
		t.Error("SharedSecretBase58 no longer catches what generateSharedSecret throws for a key of the wrong length")
	}
	// the approval's unseal is the one narrow function that opens the page's box: one try around its work and nothing
	// else in it; it is pinned whole in ur_io_approval_exact_test.go, and run against the page's own vector, with a flipped
	// byte, a wrong key and a public key of small order, by ur_io_approval_interop_test.go where the sdk is
	unseal := definitionBody(t, "ApprovalUnseal.h", files["App/ApprovalUnseal.h"], "inline std::optional<std::string> Unseal(")
	provideRequire(t, "approval::Unseal", unseal, "} catch (const std::exception&) {")

	// the check itself
	unguarded := "void F() {\n  auto d = urnet::decryptData(a, b, c);\n  g(d);\n}\n"
	if len(authLinkUnguardedThrowers(t, unguarded, true)) != 1 {
		t.Error("the check does not flag a decryptData call outside a try")
	}
	guarded := "void F() {\n  try {\n    auto d = urnet::decryptData(a, b, c);\n  } catch (const std::exception&) {\n    return;\n  }\n}\n"
	if problems := authLinkUnguardedThrowers(t, guarded, true); len(problems) != 0 {
		t.Errorf("the check flags a decryptData call inside a try: %v", problems)
	}
	wrongCatch := "void F() {\n  try {\n    auto d = urnet::decryptData(a, b, c);\n  } catch (const nlohmann::json::exception&) {\n  }\n}\n"
	if len(authLinkUnguardedThrowers(t, wrongCatch, true)) != 1 {
		t.Error("the check passes a decryptData call whose try catches something else")
	}
	nested := "void F() {\n  try {\n    if (x) {\n      auto d = urnet::generateSharedSecret(a, b);\n    }\n  } catch (const std::exception&) {\n  }\n}\n"
	if len(authLinkUnguardedThrowers(t, nested, true)) != 1 {
		t.Error("the check passes a call that sits in a block of its own inside the try")
	}
}

// The SDK calls that throw urnet::Error, in code, that are not the whole of a try
// block catching std::exception. `mustFind` makes a file with none an error.
func authLinkUnguardedThrowers(t *testing.T, code string, mustFind bool) []string {
	t.Helper()
	blanked := []byte(code)
	for i := 0; i < len(blanked); i++ {
		if blanked[i] != '"' {
			continue
		}
		for i++; i < len(blanked) && blanked[i] != '"' && blanked[i] != '\n'; i++ {
			if blanked[i] == '\\' && i+1 < len(blanked) {
				blanked[i] = ' '
				i++
			}
			blanked[i] = ' '
		}
	}
	text := string(blanked)
	var problems []string
	found := 0
	for _, call := range []string{"urnet::decryptData(", "urnet::generateSharedSecret("} {
		for from := 0; ; {
			at := strings.Index(text[from:], call)
			if at < 0 {
				break
			}
			at += from
			from = at + len(call)
			found++
			start, depth := -1, 0
			for i := at; i >= 0 && start < 0; i-- {
				switch text[i] {
				case '}':
					depth++
				case '{':
					if depth == 0 {
						start = i
					} else {
						depth--
					}
				}
			}
			guarded := false
			if start >= 0 && strings.HasSuffix(strings.TrimRight(text[:start], " \t\r\n"), "try") {
				depth = 0
				for i := start; i < len(text) && !guarded; i++ {
					switch text[i] {
					case '{':
						depth++
					case '}':
						depth--
						if depth == 0 {
							guarded = strings.HasPrefix(strings.TrimLeft(text[i+1:], " \t\r\n"), "catch (const std::exception&")
							i = len(text)
						}
					}
				}
			}
			if !guarded {
				problems = append(problems, strings.TrimSuffix(call, "(")+" on line "+strconv.Itoa(authLinkLine(text, at))+" is not inside a try that catches std::exception")
			}
		}
	}
	if found == 0 && mustFind {
		t.Fatal("no SDK crypto call found; update this contract")
	}
	return problems
}
