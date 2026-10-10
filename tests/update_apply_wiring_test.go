// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	iofs "io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the elevated update apply (app/src/Updater, the tray app's
// UpdateChecker and installer/Package.wxs). What each decision means runs in
// update_release_test.go; the helper, the tray and the installer need Windows,
// so these read their sources with every comment blanked. Each check returns
// the problems it found, so the negative controls below can run the same
// check on a rewritten source and require it to fail.

func readUpdaterSource(t *testing.T, name string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", "Updater", name))
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// The problems of `patterns` not matching in `text` in this order, each after
// the match of the one before it.
func applyOrderProblems(where, text string, patterns ...string) []string {
	from := 0
	for _, pattern := range patterns {
		location := regexp.MustCompile(pattern).FindStringIndex(text[from:])
		if location == nil {
			return []string{where + " is missing " + pattern + ", in this order after the patterns before it"}
		}
		from += location[1]
	}
	return nil
}

// The text from `opener` through the next "\n}\n", or "" when absent.
func applyDefinition(source, opener string) string {
	start := strings.Index(source, opener)
	if start < 0 {
		return ""
	}
	end := strings.Index(source[start:], "\n}\n")
	if end < 0 {
		return ""
	}
	return source[start : start+end+2]
}

func reportProblems(t *testing.T, problems []string) {
	t.Helper()
	for _, problem := range problems {
		t.Error(problem)
	}
}

// The helper is its own program: the static C runtime, imports resolved from
// System32 alone, no Common.lib and no SDK, built by the solution.
func checkUpdaterProject(project, solution, common string) []string {
	var problems []string
	for _, want := range []string{
		"<TargetName>URnetworkUpdate</TargetName>",
		"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreaded</RuntimeLibrary>",
		"<RuntimeLibrary Condition=\"'$(Configuration)'=='Debug'\">MultiThreadedDebug</RuntimeLibrary>",
		"<AdditionalOptions>/DEPENDENTLOADFLAG:0x800 %(AdditionalOptions)</AdditionalOptions>",
		`<ClCompile Include="..\Common\InstallLocationWin32.cpp" />`,
		`<ClCompile Include="ShellStart.cpp" />`,
		"<AdditionalManifestFiles>Updater.manifest</AdditionalManifestFiles>",
	} {
		if !strings.Contains(project, want) {
			problems = append(problems, "Updater.vcxproj is missing "+want)
		}
	}
	settings := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(project, "")
	for _, forbidden := range []string{"MultiThreadedDLL", "MultiThreadedDebugDLL", "Common.vcxproj",
		"URnetworkSdk", "WindowsAppSDK", "Common.lib"} {
		if strings.Contains(settings, forbidden) {
			problems = append(problems, "Updater.vcxproj names "+forbidden+": the helper must load nothing from the app's folder")
		}
	}
	for _, config := range []string{"Release|x64", "Release|ARM64"} {
		if !strings.Contains(solution, "{A1B2C3D4-0006-4E5F-8A9B-000000000006}."+config+".Build.0 = "+config) {
			problems = append(problems, "URnetwork.sln does not build the helper for "+config)
		}
	}
	if !strings.Contains(solution, `"URnetworkUpdate", "src\Updater\Updater.vcxproj", "{A1B2C3D4-0006-4E5F-8A9B-000000000006}"`) {
		problems = append(problems, "URnetwork.sln does not list the helper's project")
	}
	for _, want := range []string{`<ClCompile Include="InstallLocationWin32.cpp" />`,
		`<ClInclude Include="InstallLocationWin32.h" />`, `<ClInclude Include="UpdateApply.h" />`} {
		if !strings.Contains(common, want) {
			problems = append(problems, "Common.vcxproj is missing "+want)
		}
	}
	return problems
}

func TestUpdateApplyHelperIsItsOwnProgram(t *testing.T) {
	root := repositoryRoot(t)
	solution, err := os.ReadFile(filepath.Join(root, "app", "URnetwork.sln"))
	if err != nil {
		t.Fatal(err)
	}
	reportProblems(t, checkUpdaterProject(readUpdaterSource(t, "Updater.vcxproj"), string(solution),
		readCommonSource(t, "Common.vcxproj")))
	manifest := readUpdaterSource(t, "Updater.manifest")
	if !strings.Contains(manifest, `<requestedExecutionLevel level="asInvoker" uiAccess="false" />`) {
		t.Error("Updater.manifest must run the helper as invoked: the tray elevates it, and the relaunch must not be")
	}
}

// Before anything else the helper limits where libraries load from. Only the
// elevated mode drops the app's URNETWORK_* overrides (UpdateApply.h
// IsAppOverrideName decides which); the relaunch is the user's app, in the
// user's environment, and keeps them. It starts the app only when it is not
// itself the elevated half of a split token. Nothing in it reads the
// environment or the user's storage.
func checkUpdaterEntry(main string, sources map[string]string) []string {
	entry := applyDefinition(main, "int WINAPI wWinMain(")
	problems := applyOrderProblems("wWinMain", entry,
		regexp.QuoteMeta("::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);"),
		regexp.QuoteMeta("::CommandLineToArgvW("),
		regexp.QuoteMeta(`if (args.size() == 3 && args[1] == L"--apply-update") {`),
		regexp.QuoteMeta("DropAppOverrides();"),
		regexp.QuoteMeta("return urnw::updater::ApplyUpdate(args[2]);"),
		regexp.QuoteMeta("if (args.size() == 1) return RelaunchApp();"),
		regexp.QuoteMeta("return static_cast<int>(urnw::update::Refusal::BadArguments);"))
	if strings.Index(entry, "SetDefaultDllDirectories") > strings.Index(entry, "CommandLineToArgvW") {
		problems = append(problems, "wWinMain loads shell32 before it limits the library search")
	}
	if count := strings.Count(entry, "DropAppOverrides();"); count != 1 {
		problems = append(problems, fmt.Sprintf("wWinMain drops the app's overrides %d times, want once, for --apply-update", count))
	} else if branch := strings.Index(entry, `if (args.size() == 3 && args[1] == L"--apply-update") {`); branch < 0 ||
		strings.Index(entry, "DropAppOverrides();") < branch {
		problems = append(problems, "wWinMain drops the app's overrides before it knows the mode: the relaunch would start the app without the user's")
	}
	drop := applyDefinition(main, "void DropAppOverrides() {")
	problems = append(problems, applyOrderProblems("DropAppOverrides", drop,
		regexp.QuoteMeta("::GetEnvironmentStringsW();"),
		regexp.QuoteMeta("const std::wstring_view name = text.substr(0, equals);"),
		regexp.QuoteMeta("if (urnw::update::IsAppOverrideName(name)) names.emplace_back(name);"),
		regexp.QuoteMeta("::SetEnvironmentVariableW(name.c_str(), nullptr);"))...)
	elevated := applyDefinition(main, "bool IsFullyElevated() {")
	problems = append(problems, applyOrderProblems("IsFullyElevated", elevated,
		regexp.QuoteMeta("::GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);"),
		regexp.QuoteMeta("return !read || type == TokenElevationTypeFull;"))...)
	relaunch := applyDefinition(main, "int RelaunchApp() {")
	problems = append(problems, applyOrderProblems("RelaunchApp", relaunch,
		regexp.QuoteMeta("if (IsFullyElevated()) return 0;"),
		regexp.QuoteMeta(`folder / L"URnetwork.exe";`),
		regexp.QuoteMeta(`L"\" --after-update";`),
		regexp.QuoteMeta("::CreateProcessW(app.c_str(), command.data(),"))...)
	for name, source := range sources {
		for _, forbidden := range []string{"GetEnvironmentVariable", "getenv", "StorageRoot(", "Paths.h",
			"LoadAppPrefs", "SHGetKnownFolderPath(FOLDERID_LocalAppData"} {
			if strings.Contains(source, forbidden) {
				problems = append(problems, "the helper's "+name+" reads "+forbidden+
					": nothing a user can set may steer the elevated helper")
			}
		}
	}
	return problems
}

func updaterSources(t *testing.T) map[string]string {
	t.Helper()
	sources := map[string]string{}
	for _, name := range []string{"main.cpp", "ApplyUpdate.cpp", "Http.cpp", "HelperLog.cpp", "ShellStart.cpp"} {
		sources[name] = stripComments(readUpdaterSource(t, name))
	}
	sources["InstallLocationWin32.cpp"] = stripComments(readCommonSource(t, "InstallLocationWin32.cpp"))
	return sources
}

func TestUpdateApplyHelperStartsClean(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkUpdaterEntry(sources["main.cpp"], sources))
}

// The helper's steps, in the order the spec gives them: who and where; the
// feed's list, fetched here; the release it offers; the download through the
// one allowed redirect; the digest through the handle that holds the file;
// the package's identity; the folders, the package and the log admin-only by
// their own ACL; the image moved out of the way; msiexec; the result; and,
// after an install that failed, the relaunch.
func checkApplyUpdate(apply string) []string {
	body := applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {")
	problems := applyOrderProblems("ApplyUpdate", body,
		regexp.QuoteMeta("if (!IsElevated()) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotElevated);"),
		regexp.QuoteMeta("const fs::path executable = install::OwnExecutablePath();"),
		regexp.QuoteMeta("!install::AdminOnlyLocation(executable, why)"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotInstalled);"),
		regexp.QuoteMeta("if (version::kCode == 0) {"),
		// the lock comes before the feed and the tag, so Busy is reported
		// before a bad tag; PrepareFolder of updates\ moves up into it
		regexp.QuoteMeta(`const fs::path updates = installFolder / L"updates";`),
		regexp.QuoteMeta("Security folderSecurity(kAdminOnlyFolderSddl);"),
		regexp.QuoteMeta("Security fileSecurity(kAdminOnlyFileSddl);"),
		regexp.QuoteMeta("TakeHelperLock(updates, folderSecurity, helperLock, log)"),
		regexp.QuoteMeta("const update::Feed& feed = ChannelFeed();"),
		regexp.QuoteMeta("if (!update::IsTagArgument(feed, tag)) {"),
		regexp.QuoteMeta(`const fs::path tagFolder = updates / WidenAscii(tag);`),
		regexp.QuoteMeta("if (!PrepareFolder(tagFolder, folderSecurity, error)) {"),
		regexp.QuoteMeta(`log.Open(tagFolder / L"update-helper.log", fileSecurity.attributes());`),
		regexp.QuoteMeta("if (WriteResult(updates, result, fileSecurity, writeError)) {"),
		regexp.QuoteMeta("const std::wstring listUrl = WidenAscii(update::ReleaseListUrl(feed));"),
		regexp.QuoteMeta("HttpGet("),
		regexp.QuoteMeta("if (list.status == 403 || list.status == 429) {"),
		regexp.QuoteMeta("return refuse(Refusal::RateLimited,"),
		regexp.QuoteMeta("if (list.status != 200) {"),
		regexp.QuoteMeta("if (list.serverUnixSeconds == 0) {"),
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("update::SelectRelease(*releases, kArch, feed, list.serverUnixSeconds);"),
		regexp.QuoteMeta("if (!update::SelectionOffers(selection, tag, version::kCode)) {"),
		regexp.QuoteMeta("if (!update::IsFeedAssetUrl(feed, selection.tag, selection.assetName, selection.assetUrl)) {"),
		regexp.QuoteMeta("HttpGet(WidenAscii(selection.assetUrl), nullptr, 0, nullptr, redirect, error)"),
		regexp.QuoteMeta("if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||"),
		regexp.QuoteMeta("!HttpUrlHostIsOneOf(WidenAscii(redirect.location), update::kAssetRedirectHosts)) {"),
		regexp.QuoteMeta("GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,"),
		regexp.QuoteMeta("fileSecurity.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));"),
		regexp.QuoteMeta("WidenAscii(redirect.location), nullptr, kMaxPackageBytes,"),
		regexp.QuoteMeta("Handle held(::CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,"),
		regexp.QuoteMeta("!SameFile(writtenId, heldId)"),
		regexp.QuoteMeta("const std::string actual = Sha256(held.get());"),
		regexp.QuoteMeta("!update::EqualsAsciiCaseless(actual, selection.digestHex)"),
		regexp.QuoteMeta("return refuse(Refusal::Digest,"),
		regexp.QuoteMeta("::MsiOpenDatabaseW(package.c_str(), MSIDBOPEN_READONLY, database.put())"),
		regexp.QuoteMeta(`PackageProperty(database.get(), L"UpgradeCode");`),
		regexp.QuoteMeta(`PackageProperty(database.get(), L"ProductVersion");`),
		regexp.QuoteMeta("!update::PackageMatches(*upgradeCode, *productVersion, selection.code)"),
		regexp.QuoteMeta("return refuse(Refusal::Package,"),
		regexp.QuoteMeta(`const fs::path installLog = tagFolder / L"install.log";`),
		regexp.QuoteMeta("::DeleteFileW(installLog.c_str());"),
		regexp.QuoteMeta("fileSecurity.attributes(), CREATE_NEW,"),
		regexp.QuoteMeta("!CheckAdminOnly({updates, tagFolder, package, installLog}, error)"),
		regexp.QuoteMeta("return refuse(Refusal::Staging,"),
		regexp.QuoteMeta("::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)"),
		regexp.QuoteMeta(`const fs::path msiexec = systemFolder / L"msiexec.exe";`),
		regexp.QuoteMeta("update::MsiexecCommandLine(msiexec.native(), package.native(),"),
		regexp.QuoteMeta("::CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,"),
		regexp.QuoteMeta("::WaitForSingleObject(installer.get(), INFINITE);"),
		regexp.QuoteMeta("::GetExitCodeProcess(installer.get(), &exitCode);"),
		regexp.QuoteMeta("held.Close();"),
		regexp.QuoteMeta("if (update::KeepsPackage(exitCode)) {"),
		regexp.QuoteMeta("return finish(exitCode);"),
		regexp.QuoteMeta("::DeleteFileW(package.c_str());"),
		regexp.QuoteMeta("const int ended = finish(exitCode);"),
		regexp.QuoteMeta("StartThroughShell(executable, shellError)"),
		regexp.QuoteMeta("return ended;"))
	// what everything under updates\ is created with: owned by Administrators,
	// SYSTEM and Administrators full, Users read and execute, nothing inherited;
	// the lock file is tighter still, with nothing for Users at all, so no
	// process without administrator rights can open it, and so hold it
	for _, want := range []string{
		`constexpr wchar_t kAdminOnlyFolderSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";`,
		`constexpr wchar_t kAdminOnlyFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`,
		`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";`,
	} {
		if !strings.Contains(apply, want) {
			problems = append(problems, "ApplyUpdate.cpp does not create what it writes under updates\\ with "+want)
		}
	}
	// The lock: updates\ prepared first, then helper.lock opened for its data,
	// shared with nobody (the 0 share mode), without following a link, created
	// admin-only; a sharing violation is Busy; a thing there that is not a plain
	// file is refused; a lock already there is given its security again.
	lockFn := applyDefinition(apply, "std::optional<Refusal> TakeHelperLock(const fs::path& updates, Security& folderSecurity,")
	problems = append(problems, applyOrderProblems("TakeHelperLock", lockFn,
		regexp.QuoteMeta("if (!PrepareFolder(updates, folderSecurity, error)) {"),
		regexp.QuoteMeta(`const fs::path lockPath = updates / L"helper.lock";`),
		regexp.QuoteMeta("FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, 0,"),
		regexp.QuoteMeta("lockSecurity.attributes(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);"),
		regexp.QuoteMeta("if (opened == ERROR_SHARING_VIOLATION) {"),
		regexp.QuoteMeta("return Refusal::Busy;"),
		regexp.QuoteMeta("(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {"),
		regexp.QuoteMeta("::SetSecurityInfo("),
		regexp.QuoteMeta("return std::nullopt;"))...)
	// The lock is kept until the helper exits. TakeHelperLock closes the handle
	// only where it then refuses, and ApplyUpdate declares the handle in its own
	// scope and names it nowhere else but to take the lock: closed, moved or
	// assigned anywhere else, the lock would be let go while the installer may
	// still be running.
	closes := strings.Count(lockFn, "lock.Close();")
	refusing := len(regexp.MustCompile(`lock\.Close\(\);\s+return Refusal::\w+;`).FindAllString(lockFn, -1))
	if closes != refusing {
		problems = append(problems, fmt.Sprintf("TakeHelperLock closes the lock's handle %d times and refuses after %d of them: "+
			"the lock is to be held until the helper exits", closes, refusing))
	}
	if !strings.Contains(body, "\n  Handle helperLock;\n") {
		problems = append(problems, "ApplyUpdate does not declare helperLock in its own scope: the lock is to be held until the helper exits")
	}
	if named := strings.Count(body, "helperLock"); named != 2 {
		problems = append(problems, fmt.Sprintf("ApplyUpdate names helperLock %d times, want twice, where it declares it and where it takes "+
			"the lock: the lock is to be held until the helper exits", named))
	}
	prepare := applyDefinition(apply, "bool PrepareFolder(const fs::path& folder, Security& security, std::string& error) {")
	problems = append(problems, applyOrderProblems("PrepareFolder", prepare,
		regexp.QuoteMeta("if (::CreateDirectoryW(folder.c_str(), security.attributes())) return true;"),
		regexp.QuoteMeta("FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,"),
		regexp.QuoteMeta("(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {"),
		regexp.QuoteMeta("::SetSecurityInfo("),
		regexp.QuoteMeta("OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,"),
		regexp.QuoteMeta("owner, nullptr, dacl, nullptr);"),
		regexp.QuoteMeta("return true;"))...)
	write := applyDefinition(apply, "bool WriteResult(const fs::path& updates, const update::UpdateResult& result, Security& security,")
	problems = append(problems, applyOrderProblems("WriteResult", write,
		regexp.QuoteMeta("security.attributes(), CREATE_NEW,"),
		regexp.QuoteMeta("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH"))...)
	check := applyDefinition(apply, "bool CheckAdminOnly(const std::vector<fs::path>& paths, std::string& error) {")
	problems = append(problems, applyOrderProblems("CheckAdminOnly", check,
		regexp.QuoteMeta("for (const fs::path& path : paths) {"),
		regexp.QuoteMeta("if (!install::AdminOnlyPath(path, why)) {"),
		regexp.QuoteMeta("return false;"))...)
	return problems
}

// What the helper's admin-only checks judge: a token without admin rights,
// and the rights that change a file or a folder.
func checkAdminOnlyPath(location string) []string {
	return applyOrderProblems("AdminOnlyPath", applyDefinition(location, "bool AdminOnlyPath(const fs::path& path, std::string& why) {"),
		regexp.QuoteMeta("HANDLE raw = NonAdminToken(why);"),
		regexp.QuoteMeta("if (!RightsFor(token.get(), path, rights, why)) return false;"),
		regexp.QuoteMeta("if ((rights & kWriteRights) == 0) return true;"),
		regexp.QuoteMeta("return false;"))
}

func TestUpdateApplyHelperChecksInOrder(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkApplyUpdate(sources["ApplyUpdate.cpp"]))
	reportProblems(t, checkAdminOnlyPath(sources["InstallLocationWin32.cpp"]))
}

// Every file the helper is built from, by name, with comments blanked: all
// that lies in app/src/Updater, and the one source of Common its project
// compiles (updaterSources).
func helperFiles(t *testing.T) map[string]string {
	t.Helper()
	files := updaterSources(t)
	entries, err := os.ReadDir(filepath.Join(repositoryRoot(t), "app", "src", "Updater"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if entry.Type().IsRegular() {
			files[entry.Name()] = stripComments(readUpdaterSource(t, entry.Name()))
		}
	}
	// these two come from the listing alone: without them it saw nothing
	for _, want := range []string{"ApplyUpdate.h", "Updater.vcxproj"} {
		if _, ok := files[want]; !ok {
			t.Fatalf("the helper's files miss %s: the listing did not see app/src/Updater", want)
		}
	}
	return files
}

// The helper names no mutex, in any of its files. A named mutex is a lock any
// process on the machine can create first and hold, and for as long as it
// does, every helper ends in Busy.
func checkHelperHasNoMutex(files map[string]string) []string {
	var problems []string
	for name, text := range files {
		if strings.Contains(text, "CreateMutex") {
			problems = append(problems, "the helper's "+name+" names CreateMutex: the helper's lock is a file in updates\\, "+
				"not a mutex any process can create and hold")
		}
	}
	return problems
}

func TestUpdateApplyHelperHasNoMutex(t *testing.T) {
	reportProblems(t, checkHelperHasNoMutex(helperFiles(t)))
}

// After an install that failed, the helper starts its relaunch through the
// desktop's shell, so it runs as the signed-in user, never elevated:
// Explorer's Shell.Application object, as WixUnelevatedShellExec does.
func checkShellStart(shell string) []string {
	start := applyDefinition(shell, "bool StartThroughShell(const std::filesystem::path& program, std::string& error) {")
	problems := applyOrderProblems("StartThroughShell", start,
		regexp.QuoteMeta("::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,"),
		regexp.QuoteMeta("windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH,"),
		regexp.QuoteMeta("services->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser),"),
		regexp.QuoteMeta("browser->QueryActiveShellView(view.GetAddressOf());"),
		regexp.QuoteMeta("view->GetItemObject(SVGIO_BACKGROUND, __uuidof(IDispatch),"),
		regexp.QuoteMeta("folderView->get_Application(application.GetAddressOf());"),
		regexp.QuoteMeta("hr = application.As(&shell);"),
		regexp.QuoteMeta("hr = shell->ShellExecute(file.get(), arguments, directory, operation, show);"))
	for _, forbidden := range []string{"CreateProcess", "ShellExecuteEx", "::ShellExecuteW"} {
		if strings.Contains(shell, forbidden) {
			problems = append(problems, "ShellStart.cpp starts the relaunch with "+forbidden+
				": from the elevated helper that would run it elevated")
		}
	}
	return problems
}

func TestUpdateApplyHelperRelaunchesThroughTheShell(t *testing.T) {
	reportProblems(t, checkShellStart(updaterSources(t)["ShellStart.cpp"]))
}

// Which repository's releases the helper installs is decided by the channel's
// feed alone, and that feed is one compiled in: the channel may at most name
// an id in ReleaseSelection.h's table (FeedById), never a repository, owner,
// name or URL read at run time, and nothing in the user's registry hive can
// choose it. ApplyUpdate.cpp's ChannelFeed says what the opt-in channel's
// machine-wide store must be; this pins what holds until it lands.
func checkChannelIsAnIndex(apply string, sources map[string]string) []string {
	var problems []string
	runner := regexp.MustCompile(`(?s)#if defined\(URN_UPDATE_RUNNER_TEST_REPO_ID\)\n#define URN_UPDATER_STRINGIZE_.*?#endif\n`)
	rest := runner.ReplaceAllString(apply, "")
	if rest == apply {
		problems = append(problems, "ApplyUpdate.cpp has no runner test feed block to set apart")
	}
	for _, built := range []string{"Feed{", ".numericRepoId =", ".owner =", ".repo =", ".tagPrefix ="} {
		if strings.Contains(rest, built) {
			problems = append(problems, "ApplyUpdate.cpp builds a feed at run time ("+built+
				"): the channel may only name one compiled in")
		}
	}
	channel := applyDefinition(apply, "const update::Feed& ChannelFeed() {")
	if !regexp.MustCompile(`^const update::Feed& ChannelFeed\(\) \{\s*#if defined\(URN_UPDATE_RUNNER_TEST_REPO_ID\)\s*return kRunnerTestFeed;\s*#else\s*return update::kOfficialFeed;\s*#endif\s*\}\s*$`).MatchString(channel) {
		problems = append(problems, "ChannelFeed returns something other than the compiled-in official feed: "+
			"the opt-in channel must resolve an admin-only store's id through FeedById (see its comment)")
	}
	for name, source := range sources {
		for _, user := range []string{"HKEY_CURRENT_USER", "HKCU", "HKEY_USERS", "RegOpenCurrentUser"} {
			if strings.Contains(source, user) {
				problems = append(problems, "the helper's "+name+" reads "+user+
					": what it installs is decided where only an administrator can write")
			}
		}
	}
	return problems
}

func TestUpdateApplyTheChannelIsOnlyAnIndex(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkChannelIsAnIndex(sources["ApplyUpdate.cpp"], sources))
}

// Every response the helper reads came with redirects refused and a
// certificate the machine's own trust accepts for the host.
func checkUpdaterHttp(http string) []string {
	get := applyDefinition(http, "bool HttpGet(")
	problems := applyOrderProblems("HttpGet", get,
		regexp.QuoteMeta("parts.nScheme != INTERNET_SCHEME_HTTPS"),
		regexp.QuoteMeta("WINHTTP_FLAG_SECURE));"),
		regexp.QuoteMeta("DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;"),
		regexp.QuoteMeta("WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))"),
		regexp.QuoteMeta("::WinHttpReceiveResponse(request.get(), nullptr)"),
		regexp.QuoteMeta("if (!MachineTrusts(request.get(), host, error)) return false;"),
		regexp.QuoteMeta("WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER"),
		regexp.QuoteMeta("if (status != 200 || !sink) return true;"),
		regexp.QuoteMeta("if (total > maxBytes) {"))
	trust := applyDefinition(http, "bool MachineTrusts(")
	problems = append(problems, applyOrderProblems("MachineTrusts", trust,
		regexp.QuoteMeta("WINHTTP_OPTION_SERVER_CERT_CONTEXT"),
		regexp.QuoteMeta("::CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate, nullptr,"),
		regexp.QuoteMeta("https.dwAuthType = AUTHTYPE_SERVER;"),
		regexp.QuoteMeta("https.pwszServerName = const_cast<wchar_t*>(host.c_str());"),
		regexp.QuoteMeta("::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);"),
		regexp.QuoteMeta("if (!checked || status.dwError != 0) {"),
		regexp.QuoteMeta("return false;"))...)
	return problems
}

func TestUpdateApplyHelperTrustsOnlyTheMachine(t *testing.T) {
	reportProblems(t, checkUpdaterHttp(updaterSources(t)["Http.cpp"]))
}

// The UpgradeCode the helper requires is the one the package carries.
func TestUpdateApplyUpgradeCodeIsThePackages(t *testing.T) {
	document := parseXML(t, filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs"))
	packages := document.descendants(wixNamespace, "Package")
	if len(packages) != 1 {
		t.Fatalf("Package.wxs has %d Package elements", len(packages))
	}
	upgradeCode, _ := packages[0].attribute("UpgradeCode")
	want := `inline constexpr std::string_view kUpgradeCode = "{` + strings.ToUpper(upgradeCode) + `}";`
	if !strings.Contains(stripComments(readCommonSource(t, "UpdateApply.h")), want) {
		t.Errorf("UpdateApply.h does not require Package.wxs's UpgradeCode: want %s", want)
	}
}

// A runner test's feed reaches a build only through the four
// /p:UrnUpdateRunnerTest* properties, never from the environment, and no file
// in this repository a build reads sets one: every build script, MSBuild
// project, props, targets and response file, and workflow. A helper built
// with it says so in its FileDescription, and the release build and the
// payload check refuse to package one, whatever passed the properties.
func checkRunnerFeedIsTestOnly(inputs map[string]string, project, resources, apply string,
	sources map[string]string) []string {
	var problems []string
	feedDefault := regexp.MustCompile(`UrnUpdateFeedDefault=["']?([A-Za-z0-9_-]*)`)
	for name, text := range inputs {
		if name == "app/src/Updater/Updater.vcxproj" {
			continue
		}
		for _, setting := range []string{"UrnUpdateRunnerTest", "URN_UPDATE_RUNNER_TEST", "URN_UPDATE_FEED_DEFAULT_BETA"} {
			if strings.Contains(text, setting) {
				problems = append(problems, name+" sets "+setting+": official builds must poll the official feed")
			}
		}
		for _, match := range feedDefault.FindAllStringSubmatch(text, -1) {
			if match[1] != "official" {
				problems = append(problems, name+" passes UrnUpdateFeedDefault="+match[1]+
					": official builds must poll the official feed")
			}
		}
	}
	if !strings.Contains(project, `<ItemDefinitionGroup Condition="'$(UrnUpdateRunnerTestRepoId)'!=''">`) {
		problems = append(problems, "Updater.vcxproj defines a runner feed without its property")
	}
	if regexp.MustCompile(`<UrnUpdateRunnerTest[A-Za-z]*>`).MatchString(project) {
		problems = append(problems, "Updater.vcxproj gives a runner feed property a value of its own")
	}
	if !strings.Contains(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))") {
		problems = append(problems, "Updater.vcxproj takes a runner feed from the environment")
	}
	if !strings.Contains(project, "<ResourceCompile>\n      <PreprocessorDefinitions>URN_UPDATE_RUNNER_TEST_REPO_ID=$(UrnUpdateRunnerTestRepoId);") {
		problems = append(problems, "Updater.vcxproj does not tell Updater.rc that a runner feed is compiled in")
	}
	if !strings.Contains(resources, "#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)\n"+
		`            VALUE "FileDescription",  "URnetwork update (runner test feed)"`+"\n#else\n"+
		`            VALUE "FileDescription",  "URnetwork update"`+"\n#endif") {
		problems = append(problems, "Updater.rc does not mark a helper built with a runner feed")
	}
	build := inputs["app/build.ps1"]
	if marked := strings.Index(build, `$helperDescription = [Diagnostics.FileVersionInfo]::GetVersionInfo($helper).FileDescription`); marked < 0 ||
		!strings.Contains(build, `if ($helperDescription -ne "URnetwork update") {`) ||
		strings.Index(build, "dotnet @wixArgs") < marked {
		problems = append(problems, "app/build.ps1 packages an update helper without checking that it is the official feed's")
	}
	verify := inputs["app/tools/verify-msi-payload.ps1"]
	if !strings.Contains(verify, `$description = [Diagnostics.FileVersionInfo]::GetVersionInfo($extracted).FileDescription`) ||
		!strings.Contains(verify, `if ($description -ne "URnetwork update") {`) {
		problems = append(problems, "verify-msi-payload.ps1 passes an MSI without checking its update helper's feed")
	}
	if !strings.Contains(apply, "#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)\n  return kRunnerTestFeed;\n#else") {
		problems = append(problems, "ApplyUpdate.cpp reads the runner feed outside its define")
	}
	if strings.Count(apply, "kRunnerTestFeed") != 2 {
		problems = append(problems, "ApplyUpdate.cpp uses kRunnerTestFeed somewhere other than the channel's feed")
	}
	for name, source := range sources {
		if name != "ApplyUpdate.cpp" && strings.Contains(source, "URN_UPDATE_RUNNER_TEST") {
			problems = append(problems, name+" reads a runner feed: only the helper's channel may")
		}
	}
	return problems
}

// Every file in the repository a build reads, by kind, keyed by its slash
// path: build and test scripts, MSBuild projects, props, targets and
// response files, the solution, and workflows. Build output folders are
// skipped.
func buildInputs(t *testing.T) map[string]string {
	t.Helper()
	root := repositoryRoot(t)
	kinds := map[string]bool{".props": true, ".targets": true, ".rsp": true, ".vcxproj": true, ".sln": true,
		".wixproj": true, ".ps1": true, ".psm1": true, ".cmd": true, ".bat": true, ".sh": true, ".yml": true,
		".yaml": true}
	skip := map[string]bool{".git": true, "build": true, "bin": true, "obj": true, "packages": true,
		"node_modules": true, "vcpkg_installed": true, ".vs": true}
	inputs := map[string]string{}
	err := filepath.WalkDir(root, func(path string, entry iofs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			if path != root && skip[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if !kinds[strings.ToLower(filepath.Ext(entry.Name()))] {
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
		inputs[filepath.ToSlash(relative)] = string(data)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"app/build.ps1", "app/Directory.Build.props", "app/URnetwork.sln",
		"app/src/Updater/Updater.vcxproj", "app/tools/verify-msi-payload.ps1"} {
		if _, ok := inputs[want]; !ok {
			t.Fatalf("the build inputs miss %s: the walk did not see the repository", want)
		}
	}
	return inputs
}

func TestUpdateApplyRunnerFeedIsTestOnly(t *testing.T) {
	sources := updaterSources(t)
	appSources := appSourceFiles(t, ".cpp", ".h")
	for name, source := range appSources {
		sources["App/"+name] = stripComments(source)
	}
	reportProblems(t, checkRunnerFeedIsTestOnly(buildInputs(t), readUpdaterSource(t, "Updater.vcxproj"),
		readUpdaterSource(t, "Updater.rc"), sources["ApplyUpdate.cpp"], sources))
}

// Each check above fails on a source that drops the defence it pins.
func TestUpdateApplyWiringRejectsWeakerHelpers(t *testing.T) {
	root := repositoryRoot(t)
	solutionData, err := os.ReadFile(filepath.Join(root, "app", "URnetwork.sln"))
	if err != nil {
		t.Fatal(err)
	}
	project := readUpdaterSource(t, "Updater.vcxproj")
	resources := readUpdaterSource(t, "Updater.rc")
	solution := string(solutionData)
	common := readCommonSource(t, "Common.vcxproj")
	sources := updaterSources(t)
	inputs := buildInputs(t)
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	with := func(name, text string) map[string]string {
		copied := map[string]string{}
		for key, value := range sources {
			copied[key] = value
		}
		copied[name] = text
		return copied
	}
	withInput := func(name, text string) map[string]string {
		copied := map[string]string{}
		for key, value := range inputs {
			copied[key] = value
		}
		copied[name] = text
		return copied
	}
	runnerFeed := func(changed map[string]string) []string {
		return checkRunnerFeedIsTestOnly(changed, project, resources, sources["ApplyUpdate.cpp"], sources)
	}
	apply := func(old, replacement string) []string {
		return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"], old, replacement))
	}
	helper := helperFiles(t)
	mutexIn := func(name, old, replacement string) []string {
		changed := map[string]string{}
		for key, value := range helper {
			changed[key] = value
		}
		changed[name] = replace(helper[name], old, replacement)
		return checkHelperHasNoMutex(changed)
	}
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"the UpgradeCode check skipped", func() []string {
			return apply("!update::PackageMatches(*upgradeCode, *productVersion, selection.code)", "false")
		}},
		{"the digest compared to nothing", func() []string {
			return apply("!update::EqualsAsciiCaseless(actual, selection.digestHex)", "false")
		}},
		{"any offered release installed", func() []string {
			return apply("if (!update::SelectionOffers(selection, tag, version::kCode)) {", "if (selection.code == 0) {")
		}},
		{"a redirect anywhere", func() []string {
			return apply("if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||",
				"if (redirect.status != 302 ||")
		}},
		{"hashed after the handle is gone", func() []string {
			return apply("const std::string actual = Sha256(held.get());", "const std::string actual = HashPath(package);")
		}},
		{"the image left in the installer's way", func() []string {
			return apply("::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)", "false")
		}},
		{"a user-writable location", func() []string {
			return apply("!install::AdminOnlyLocation(executable, why)", "false")
		}},
		{"GitHub's limit taken for a broken list", func() []string {
			return apply("if (list.status == 403 || list.status == 429) {", "if (false) {")
		}},
		{"the repair source left to what it inherits", func() []string {
			return apply("fileSecurity.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));",
				"nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));")
		}},
		{"msiexec's log left to msiexec", func() []string {
			return apply("fileSecurity.attributes(), CREATE_NEW,\n", "nullptr, CREATE_NEW,\n")
		}},
		{"what the helper writes owned by whoever creates it", func() []string {
			// anchored on the whole file SDDL: the lock file's SDDL now shares
			// the O:BAD:P(A;;FA;;;SY) prefix, so this drops the owner from
			// kAdminOnlyFileSddl in particular
			return apply(`L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`, `L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`)
		}},
		{"a folder inheriting what is above it", func() []string {
			return apply(`L"O:BAD:P(A;OICI;FA;;;SY)`, `L"O:BAD:(A;OICI;FA;;;SY)`)
		}},
		{"an existing folder's security kept", func() []string {
			return apply("const DWORD set = ::SetSecurityInfo(", "const DWORD set = ERROR_SUCCESS; (void)(")
		}},
		// The lock, each control restoring one rejected design.
		{"the lock taken as a global mutex again", func() []string {
			return mutexIn("ApplyUpdate.cpp", "const HANDLE raw = ::CreateFileW(",
				`const HANDLE raw = ::CreateMutexW(nullptr, FALSE, L"Global\\URnetworkUpdateHelper"); const HANDLE unused = ::CreateFileW(`)
		}},
		{"a global mutex taken in another file of the helper", func() []string {
			return mutexIn("main.cpp", "    DropAppOverrides();\n",
				"    DropAppOverrides();\n    ::CreateMutexW(nullptr, FALSE, L\"Global\\\\URnetworkUpdateHelper\");\n")
		}},
		{"the lock file shared for read", func() []string {
			return apply("FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, 0,",
				"FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,")
		}},
		{"the lock file lets Users open it for data", func() []string {
			return apply(`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";`,
				`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";`)
		}},
		{"the lock taken before the folder is prepared", func() []string {
			// the same text, moved below the lock: only its place changes
			prepared := "  std::string error;\n  if (!PrepareFolder(updates, folderSecurity, error)) {\n    log.Line(\"refused: {}\", error);\n    return Refusal::Staging;\n  }\n"
			moved := replace(sources["ApplyUpdate.cpp"], prepared+"  Security lockSecurity(kLockFileSddl);",
				"  Security lockSecurity(kLockFileSddl);")
			return checkApplyUpdate(replace(moved, "  return std::nullopt;\n}", prepared+"  return std::nullopt;\n}"))
		}},
		{"a link to the lock file followed", func() []string {
			return apply("lockSecurity.attributes(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);",
				"lockSecurity.attributes(), OPEN_ALWAYS, 0, nullptr);")
		}},
		{"the lock let go as soon as it is taken", func() []string {
			return apply("  return std::nullopt;\n}", "  lock.Close();\n  return std::nullopt;\n}")
		}},
		{"the lock let go while the installer runs", func() []string {
			return apply("  ::WaitForSingleObject(installer.get(), INFINITE);\n",
				"  helperLock.Close();\n  ::WaitForSingleObject(installer.get(), INFINITE);\n")
		}},
		{"the lock's handle ending before the helper does", func() []string {
			return apply("  Handle helperLock;\n  if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n    return static_cast<int>(*refusal);\n  }\n",
				"  {\n    Handle helperLock;\n    if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n      return static_cast<int>(*refusal);\n    }\n  }\n")
		}},
		{"the rights not checked before msiexec", func() []string {
			return apply("!CheckAdminOnly({updates, tagFolder, package, installLog}, error)", "false")
		}},
		{"a non-admin's rights not judged", func() []string {
			return checkAdminOnlyPath(replace(sources["InstallLocationWin32.cpp"],
				"if ((rights & kWriteRights) == 0) return true;", "return true;"))
		}},
		{"no relaunch after an install that failed", func() []string {
			return apply("} else if (StartThroughShell(executable, shellError)) {", "} else if (false) {")
		}},
		{"the relaunch before the report", func() []string {
			moved := replace(sources["ApplyUpdate.cpp"], "  const int ended = finish(exitCode);\n", "")
			return checkApplyUpdate(replace(moved, "  return ended;\n}", "  const int ended = finish(exitCode);\n  return ended;\n}"))
		}},
		{"the relaunch started elevated by the helper", func() []string {
			return checkShellStart(replace(sources["ShellStart.cpp"],
				"hr = shell->ShellExecute(file.get(), arguments, directory, operation, show);",
				"hr = ::CreateProcessW(program.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, nullptr, nullptr) ? S_OK : E_FAIL;"))
		}},
		{"redirects followed", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;", "DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;"))
		}},
		{"the user's roots trusted", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"::CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate, nullptr,",
				"::CertGetCertificateChain(nullptr, certificate, nullptr,"))
		}},
		{"no certificate check", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"if (!MachineTrusts(request.get(), host, error)) return false;", ""))
		}},
		{"the dynamic C runtime", func() []string {
			return checkUpdaterProject(replace(project,
				"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreaded</RuntimeLibrary>",
				"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreadedDLL</RuntimeLibrary>"),
				solution, common)
		}},
		{"imports searched in the app's folder", func() []string {
			return checkUpdaterProject(replace(project, "/DEPENDENTLOADFLAG:0x800 ", ""), solution, common)
		}},
		{"libraries loaded before the search is limited", func() []string {
			main := replace(sources["main.cpp"],
				"  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);\n", "")
			main = replace(main, "  if (!argv) return",
				"  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);\n  if (!argv) return")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"an app override read", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "  const fs::path installFolder = executable.parent_path();",
				"  const fs::path installFolder = _wgetenv(L\"URNETWORK_APP_ROOT\");")
			return checkUpdaterEntry(sources["main.cpp"], with("ApplyUpdate.cpp", changed))
		}},
		{"the user's overrides dropped for the relaunch too", func() []string {
			main := replace(sources["main.cpp"], "    DropAppOverrides();\n", "")
			main = replace(main, "  int argc = 0;", "  DropAppOverrides();\n  int argc = 0;")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"the relaunch started elevated", func() []string {
			main := replace(sources["main.cpp"], "  if (IsFullyElevated()) return 0;\n", "")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"the relaunch's elevation test inverted", func() []string {
			main := replace(sources["main.cpp"], "return !read || type == TokenElevationTypeFull;",
				"return !read || type == TokenElevationTypeLimited;")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"a channel read from the user's registry", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "#else\n  return update::kOfficialFeed;\n#endif",
				"#else\n  ::RegGetValueW(HKEY_CURRENT_USER, L\"Software\\\\URnetwork\", L\"Channel\", RRF_RT_REG_SZ, nullptr, nullptr, nullptr);\n  return update::kOfficialFeed;\n#endif")
			return checkChannelIsAnIndex(changed, with("ApplyUpdate.cpp", changed))
		}},
		{"a feed built at run time", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "#else\n  return update::kOfficialFeed;\n#endif",
				"#else\n  static const update::Feed stored{.id = \"stored\", .numericRepoId = 1, .owner = \"someone\", .repo = \"else\"};\n  return stored;\n#endif")
			return checkChannelIsAnIndex(changed, with("ApplyUpdate.cpp", changed))
		}},
		{"build.ps1 passes a runner feed", func() []string {
			return runnerFeed(withInput("app/build.ps1", inputs["app/build.ps1"]+"\n/p:UrnUpdateRunnerTestRepoId=1\n"))
		}},
		{"build.ps1 passes another feed", func() []string {
			return runnerFeed(withInput("app/build.ps1", inputs["app/build.ps1"]+"\n/p:UrnUpdateFeedDefault=beta\n"))
		}},
		{"a workflow passes a runner feed", func() []string {
			return runnerFeed(withInput(".github/workflows/extra.yml", "run: msbuild /p:UrnUpdateRunnerTestRepoId=1"))
		}},
		{"a props file sets the runner feed", func() []string {
			return runnerFeed(withInput("app/Directory.Build.props", inputs["app/Directory.Build.props"]+
				"\n<PropertyGroup><UrnUpdateRunnerTestRepoId>1</UrnUpdateRunnerTestRepoId></PropertyGroup>\n"))
		}},
		{"a response file sets the runner feed", func() []string {
			return runnerFeed(withInput("app/Directory.Build.rsp", "/p:UrnUpdateRunnerTestRepoId=1"))
		}},
		{"build.ps1 packages a runner helper", func() []string {
			return runnerFeed(withInput("app/build.ps1", replace(inputs["app/build.ps1"],
				`if ($helperDescription -ne "URnetwork update") {`, `if ($false) {`)))
		}},
		{"the payload check passes a runner helper", func() []string {
			return runnerFeed(withInput("app/tools/verify-msi-payload.ps1", replace(inputs["app/tools/verify-msi-payload.ps1"],
				`if ($description -ne "URnetwork update") {`, `if ($false) {`)))
		}},
		{"a runner helper that does not say so", func() []string {
			return checkRunnerFeedIsTestOnly(inputs, project, replace(resources, " (runner test feed)", ""),
				sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed from the environment", func() []string {
			return checkRunnerFeedIsTestOnly(inputs,
				strings.ReplaceAll(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))", ""),
				resources, sources["ApplyUpdate.cpp"], sources)
		}},
		{"Updater.vcxproj gives the runner feed a value", func() []string {
			return checkRunnerFeedIsTestOnly(inputs, project+
				"\n<PropertyGroup><UrnUpdateRunnerTestRepoId>1</UrnUpdateRunnerTestRepoId></PropertyGroup>\n",
				resources, sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed as the default", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "  return update::kOfficialFeed;\n", "  return kRunnerTestFeed;\n")
			return checkRunnerFeedIsTestOnly(inputs, project, resources, changed, sources)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The tray app hands an elevated process nothing but a tag: on an installed
// copy it starts the helper beside it and waits; on any other copy, and when
// the user asks for the installer, it shows the checked installer and
// elevates nothing. It never builds msiexec's arguments, and it does not quit
// for the installer: the installer closes it, and only then, as it exits,
// does it record the helper in the update marker. Its wait ends with the
// helper or with the app's own exit, whichever is first.
func checkTrayRunsTheHelper(checker string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const fs::path exe = install::OwnExecutablePath();"),
		regexp.QuoteMeta("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);"),
		regexp.QuoteMeta("snapshot_.installed = installed_;"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return stop_;\n  };"),
		regexp.QuoteMeta("const fs::path dir = UpdatesDir() / offer.tag;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(msiPath);"),
		regexp.QuoteMeta("fail(Failure::Checksum);"),
		regexp.QuoteMeta("if (!viaHelper) {"),
		regexp.QuoteMeta("s.phase = Phase::ManualInstall;"),
		regexp.QuoteMeta("RevealInExplorer(msiW);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("fail(refusal == kElevationRefusedUnsigned ? Failure::Unsigned : Failure::Elevation);"),
		regexp.QuoteMeta("[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);"),
		regexp.QuoteMeta("if (waited == update::HelperWait::AppExiting) {"),
		regexp.QuoteMeta("RecordUpdateInProgress(helper);"),
		regexp.QuoteMeta("::CloseHandle(helper);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	if portable := strings.Index(apply, "if (!viaHelper) {"); portable >= 0 {
		branch := apply[portable:]
		if end := strings.Index(branch, "\n  }\n"); end >= 0 {
			branch = branch[:end]
		}
		for _, forbidden := range []string{"LaunchUpdateHelper", "runas", "ShellExecute"} {
			if strings.Contains(branch, forbidden) {
				problems = append(problems, "the installer branch of RunApply runs "+forbidden+
					": a copy outside an admin-only install, or a user who asked for the installer, elevates nothing")
			}
		}
	}
	launch := applyDefinition(checker,
		"bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,")
	problems = append(problems, applyOrderProblems("LaunchUpdateHelper", launch,
		regexp.QuoteMeta(`const std::wstring params = L"--apply-update " + tag;`),
		regexp.QuoteMeta(`sei.lpVerb = L"runas";`),
		regexp.QuoteMeta("sei.lpFile = helperPath.c_str();"),
		regexp.QuoteMeta("sei.lpParameters = params.c_str();"),
		regexp.QuoteMeta("*refusal = ::GetLastError();"),
		regexp.QuoteMeta("*helper = sei.hProcess;"))...)
	if strings.Contains(launch, "RecordUpdateInProgress") {
		problems = append(problems, "LaunchUpdateHelper records the helper as it starts: launches during the "+
			"helper's download would be refused while this app still runs to serve them")
	}
	if !strings.Contains(checker, "constexpr DWORD kElevationRefusedUnsigned = ERROR_DS_REFERRAL;") {
		problems = append(problems, "UpdateChecker.cpp does not name the elevation's answer for an unsigned program")
	}
	if !strings.Contains(checker, `constexpr wchar_t kHelperName[] = L"URnetworkUpdate.exe";`) {
		problems = append(problems, "UpdateChecker.cpp does not name the helper URnetworkUpdate.exe")
	}
	for _, forbidden := range []string{"msiexec", "/passive", "/l*v", "SetInstallerStartedHandler",
		"InstallerStarted"} {
		if strings.Contains(checker, forbidden) {
			problems = append(problems, "UpdateChecker.cpp has "+forbidden+
				": only the elevated helper builds msiexec's command line, and the app does not quit for it")
		}
	}
	return problems
}

func TestUpdateApplyTrayRunsTheHelper(t *testing.T) {
	reportProblems(t, checkTrayRunsTheHelper(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// The helper's report is read from the admin-only install folder only, and
// shown until the user dismisses it, for as long as it is still true of the
// build that reads it (UpdateResult.h ViewOfReport): a 3010 until Windows has
// restarted, a failure until this build reaches that release. After the
// helper this app waited on ends, it is that run's report only when its tag,
// its exit code and its time say so; otherwise the exit code alone.
func checkTrayReadsTheReport(checker string) []string {
	var problems []string
	show := applyDefinition(checker, "void UpdateChecker::ShowLastResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ShowLastResult", show,
		regexp.QuoteMeta("if (!installed_) return;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("if (SeenResult() == result->finishedUtc) return;"),
		regexp.QuoteMeta("const std::optional<std::int64_t> finished = update::ParseUtcSecond(result->finishedUtc);"),
		regexp.QuoteMeta("const bool restartedSince = finished && BootUnixSeconds() > *finished;"),
		`update::ViewOfReport\(\*result, version::kCode,\s*false, restartedSince\);`,
		regexp.QuoteMeta("if (view == update::ReportView::Hidden) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("s.phase = Phase::Result;"),
		regexp.QuoteMeta(".view = view};"))...)
	boot := applyDefinition(checker, "std::int64_t BootUnixSeconds() {")
	problems = append(problems, applyOrderProblems("BootUnixSeconds", boot,
		regexp.QuoteMeta("return NowUnixSeconds() - static_cast<std::int64_t>(::GetTickCount64() / 1000);"))...)
	read := applyDefinition(checker, "std::optional<update::UpdateResult> ReadResult(fs::path const& file) {")
	problems = append(problems, applyOrderProblems("ReadResult", read,
		regexp.QuoteMeta("return update::ParseUpdateResult(text);"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const std::int64_t started = NowUnixSeconds();"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("const std::int64_t ended = read ? static_cast<std::int64_t>(exitCode) : 1603;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("if (report && update::IsReportOfRun(*report, outcome.tag, ended, started)) {"),
		`result\.view = update::ViewOfReport\(outcome, version::kCode,\s*true,\s*false\);`,
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("ShowLastResult();"), regexp.QuoteMeta("CleanupStaleFiles();"))...)
	dismiss := applyDefinition(checker, "void UpdateChecker::DismissResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::DismissResult", dismiss,
		regexp.QuoteMeta("if (!finished.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(finished));"))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {"))...)
	return problems
}

func TestUpdateApplyTrayReadsTheReport(t *testing.T) {
	reportProblems(t, checkTrayReadsTheReport(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// A release the helper did not install is not a dead end. The report's
// banner offers that release's installer while a check still offers it
// (OffersInstaller), and can be closed; the portable path then runs on any
// copy, reusing the checked download the tray keeps until the update has
// taken; a Windows that elevates only signed programs is offered the
// installer too. The installer shown from the user's folder is checked again
// before every "Show file", since any of the user's processes can write it.
// A release the helper refused as no longer offered is followed by a check,
// run there, so the banner says what the feed offers now
// (update_offer_wiring_test.go pins what that check's answer does).
func checkTrayShowsTheInstaller(checker, window, connect string) []string {
	var problems []string
	offers := applyDefinition(checker, "bool UpdateChecker::OffersInstaller(Snapshot const& snapshot) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::OffersInstaller", offers,
		regexp.QuoteMeta("return snapshot.phase == Phase::Result &&"),
		regexp.QuoteMeta("snapshot.result.view == update::ReportView::NotInstalled && snapshot.offeredCode != 0 &&"),
		regexp.QuoteMeta("snapshot.offeredCode == snapshot.code;"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (applyRequested_ || manualRequested_) {"),
		regexp.QuoteMeta("const bool manual = manualRequested_ && !applyRequested_;"),
		regexp.QuoteMeta("RunApply(generation, manual);"),
		regexp.QuoteMeta("if (revealRequested_) {"),
		regexp.QuoteMeta("RunReveal(generation);"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("(manual && snapshot_.phase == Phase::Result);"),
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("if (IsCheckedDownload(msiPath, offer.digestHex)) {"),
		regexp.QuoteMeta("LaunchUpdateHelper("),
		regexp.QuoteMeta("if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);"),
		regexp.QuoteMeta("if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {"),
		regexp.QuoteMeta("RunCheck(generation);"),
		regexp.QuoteMeta("s.offeredCode = offer_.code;"))...)
	if handOff := strings.Index(apply, "LaunchUpdateHelper("); handOff >= 0 {
		if kept := strings.Index(apply, "if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);"); kept > handOff &&
			strings.Contains(apply[handOff:kept], "remove_all(dir") {
			problems = append(problems, "UpdateChecker::RunApply deletes the checked download before the helper's report says the update took")
		}
	}
	reveal := applyDefinition(checker, "void UpdateChecker::RunReveal(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunReveal", reveal,
		regexp.QuoteMeta("feedGeneration_ != generation || offer_.code != snapshot_.code) {"),
		regexp.QuoteMeta("digest = offer_.digestHex;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(installer);"),
		regexp.QuoteMeta("if (digest.empty() || actual.empty() || !update::EqualsAsciiCaseless(actual, digest)) {"),
		regexp.QuoteMeta("fs::remove(installer, ec);"),
		regexp.QuoteMeta("s.failure = Failure::Checksum;"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("RevealInExplorer(installer);"))...)
	action := applyDefinition(window, "void MainWindow::OnUpdateBannerAction() {")
	problems = append(problems, applyOrderProblems("MainWindow::OnUpdateBannerAction", action,
		regexp.QuoteMeta("if (updateSnapshot_.failure == Failure::Unsigned && updateSnapshot_.phase == Phase::Failed)"),
		regexp.QuoteMeta("urnw::pages::Updates().ShowInstaller();"),
		regexp.QuoteMeta("urnw::pages::Updates().BeginApply();"),
		regexp.QuoteMeta("case Phase::ManualInstall:"),
		regexp.QuoteMeta("urnw::pages::Updates().RevealInstaller();"),
		regexp.QuoteMeta("case Phase::Result:"),
		regexp.QuoteMeta("if (urnw::UpdateChecker::OffersInstaller(updateSnapshot_))"),
		regexp.QuoteMeta("urnw::pages::Updates().ShowInstaller();"),
		regexp.QuoteMeta("urnw::pages::Updates().DismissResult();"))...)
	if strings.Contains(window, "UpdateChecker::RevealInExplorer(") {
		problems = append(problems, "MainWindow shows the installer from the user's folder without checking it again")
	}
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta("UpdateBar().Closed("),
		regexp.QuoteMeta("if (args.Reason() != Microsoft::UI::Xaml::Controls::InfoBarCloseReason::CloseButton) return;"),
		regexp.QuoteMeta("self->updateSnapshot_.phase == urnw::UpdateChecker::Phase::Result"),
		regexp.QuoteMeta("urnw::pages::Updates().DismissResult();"))...)
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta("bar.IsClosable(urnw::UpdateChecker::OffersInstaller(snap));"),
		regexp.QuoteMeta(`action = urnw::UpdateChecker::OffersInstaller(snap) ? winrt::hstring{L"Show the installer"}`),
		regexp.QuoteMeta("case Failure::Unsigned:"),
		regexp.QuoteMeta(`action = winrt::hstring{L"Download the installer"};`))...)
	return problems
}

func TestUpdateApplyTrayShowsTheInstallerWhenTheHelperCannot(t *testing.T) {
	reportProblems(t, checkTrayShowsTheInstaller(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// The banner says what the update does: the VPN disconnects while it
// installs and is not reconnected by itself; the helper's run is its own
// stage; a copy that does not update itself is one outside Program Files;
// and how it went, as the report reads for this build.
func checkTraySaysWhatTheUpdateDoes(connect string) []string {
	var problems []string
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta(`message += L" The VPN disconnects while it installs; connect again once URnetwork is back.";`),
		regexp.QuoteMeta(`message = L"This copy of URnetwork is not installed in Program Files, so it does not "`),
		regexp.QuoteMeta("case Stage::Helper:"),
		regexp.QuoteMeta(`message = L"Installing as administrator: the update is downloaded again and checked "`),
		regexp.QuoteMeta("case Failure::Held:"))...)
	result := applyDefinition(connect, "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity ConnectPage::ApplyUpdateResult(")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateResult", result,
		regexp.QuoteMeta(`L"The VPN was disconnected for the update; connect again to protect this device.";`),
		regexp.QuoteMeta("switch (result.view) {"),
		regexp.QuoteMeta("case ReportView::Installed:"),
		regexp.QuoteMeta("message = kReconnect;"),
		regexp.QuoteMeta("case ReportView::RestartApp:"),
		regexp.QuoteMeta("case ReportView::RestartWindows:"),
		regexp.QuoteMeta("if (urnw::update::OutcomeOf(result.exitCode) != urnw::update::Outcome::Refused) {"),
		regexp.QuoteMeta("kReconnect, instead);"),
		regexp.QuoteMeta("case Refusal::RateLimited:"))...)
	return problems
}

func TestUpdateApplyTraySaysWhatTheUpdateDoes(t *testing.T) {
	reportProblems(t, checkTraySaysWhatTheUpdateDoes(stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// The relaunch after an update is the one launch the update marker must not
// turn away: it carries --after-update and waits, bounded, for the update to
// end before it asks like every launch. The wait is InstanceHandover.h's
// AwaitUpdateEnd (instance_handover_test.go runs it against a fake clock),
// bound here to the marker, the steady clock and Sleep.
func checkRelaunchWaits(main, glue, handover string) []string {
	var problems []string
	for _, want := range []string{
		`inline constexpr std::wstring_view kAfterUpdateArgument = L"--after-update";`,
		`inline constexpr std::chrono::milliseconds kAfterUpdateBudget{120000};`,
		`inline constexpr std::chrono::milliseconds kAfterUpdatePoll{500};`,
	} {
		if !strings.Contains(handover, want) {
			problems = append(problems, "InstanceHandover.h no longer has "+want)
		}
	}
	wait := applyDefinition(handover, "bool AwaitUpdateEnd(Updating&& updating, Now&& now, Sleep&& sleep,")
	problems = append(problems, applyOrderProblems("instance::AwaitUpdateEnd", wait,
		regexp.QuoteMeta("const std::chrono::milliseconds deadline = now() + budget;"),
		regexp.QuoteMeta("while (updating()) {"),
		regexp.QuoteMeta("if (now() >= deadline) return false;"),
		regexp.QuoteMeta("sleep(kAfterUpdatePoll);"),
		regexp.QuoteMeta("return true;"))...)
	entry := applyDefinition(main, "int __stdcall wWinMain(")
	problems = append(problems, applyOrderProblems("wWinMain", entry,
		regexp.QuoteMeta("if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();"),
		regexp.QuoteMeta("urnw::CreateExitingSignal();"),
		regexp.QuoteMeta("urnw::instance::Launch(launcher);"))...)
	problems = append(problems, applyOrderProblems("LaunchedAfterUpdate",
		applyDefinition(glue, "bool LaunchedAfterUpdate() {"),
		regexp.QuoteMeta("return instance::HasArgument(::GetCommandLineW(), instance::kAfterUpdateArgument);"))...)
	problems = append(problems, applyOrderProblems("AwaitUpdateEnd",
		applyDefinition(glue, "void AwaitUpdateEnd() {"),
		regexp.QuoteMeta("const bool ended = instance::AwaitUpdateEnd("),
		regexp.QuoteMeta("[] { return UpdateInProgress(); },"),
		regexp.QuoteMeta("std::chrono::steady_clock::now().time_since_epoch());"),
		regexp.QuoteMeta("[](std::chrono::milliseconds pause) { ::Sleep(static_cast<DWORD>(pause.count())); });"))...)
	return problems
}

func TestUpdateApplyTheRelaunchWaitsForTheUpdate(t *testing.T) {
	reportProblems(t, checkRelaunchWaits(appMainSource(t), stripComments(readAppSource(t, "SingleInstance.cpp")),
		stripComments(readCommonSource(t, "InstanceHandover.h"))))
}

// Each tray check fails on a source that drops what it pins.
func TestUpdateApplyWiringRejectsWeakerTrays(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	main := appMainSource(t)
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	handover := stripComments(readCommonSource(t, "InstanceHandover.h"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	runs := func(old, replacement string) []string {
		return checkTrayRunsTheHelper(replace(checker, old, replacement))
	}
	reads := func(old, replacement string) []string {
		return checkTrayReadsTheReport(replace(checker, old, replacement))
	}
	shows := func(old, replacement string) []string {
		return checkTrayShowsTheInstaller(replace(checker, old, replacement), window, connect)
	}
	says := func(old, replacement string) []string {
		return checkTraySaysWhatTheUpdateDoes(replace(connect, old, replacement))
	}
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"a portable copy runs the helper", func() []string {
			return runs("const bool viaHelper = installed_ && !manual;", "const bool viaHelper = !manual;")
		}},
		{"the helper started from the installer branch", func() []string {
			return runs("    RevealInExplorer(msiW);\n    return;\n",
				"    LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal, launchError);\n    return;\n")
		}},
		{"any copy treated as installed", func() []string {
			return runs("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);",
				"installed_ = !exe.empty();")
		}},
		{"the tray builds msiexec's arguments", func() []string {
			return runs(`const std::wstring params = L"--apply-update " + tag;`,
				`const std::wstring params = L"/i msiexec --apply-update " + tag;`)
		}},
		{"a wait the app's teardown cannot end", func() []string {
			return runs("  const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return stop_;\n  };",
				"  const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return false;\n  };")
		}},
		{"a wait that never looks at the app's exit", func() []string {
			return runs("[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);",
				"[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, [] { return false; });")
		}},
		{"the marker written as the helper starts", func() []string {
			return runs("    *helper = sei.hProcess;\n", "    RecordUpdateInProgress(sei.hProcess);\n    *helper = sei.hProcess;\n")
		}},
		{"no marker when the app exits for the installer", func() []string {
			return runs("    RecordUpdateInProgress(helper);\n", "")
		}},
		{"an unsigned helper taken for a declined prompt", func() []string {
			return runs("fail(refusal == kElevationRefusedUnsigned ? Failure::Unsigned : Failure::Elevation);",
				"fail(Failure::Elevation);")
		}},
		{"a report read on a portable copy", func() []string {
			return reads("void UpdateChecker::ShowLastResult() {\n  if (!installed_) return;\n",
				"void UpdateChecker::ShowLastResult() {\n")
		}},
		{"a report dropped by the next check", func() []string {
			return reads("if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {",
				"if (snapshot_.phase != Phase::None) {")
		}},
		{"a report shown whatever build reads it", func() []string {
			return reads("if (view == update::ReportView::Hidden) {", "if (false) {")
		}},
		{"a restart asked for after Windows restarted", func() []string {
			return reads("const bool restartedSince = finished && BootUnixSeconds() > *finished;",
				"const bool restartedSince = false;")
		}},
		{"an earlier attempt's report taken as this one's", func() []string {
			return reads("if (report && update::IsReportOfRun(*report, outcome.tag, ended, started)) {",
				"if (report && report->tag == outcome.tag) {")
		}},
		{"a release that did not install with no way out", func() []string {
			return shows("snapshot.offeredCode == snapshot.code;", "false;")
		}},
		{"the checked download deleted as the helper starts", func() []string {
			return shows("  if (!helper) return;\n", "  if (!helper) return;\n  fs::remove_all(dir, ec);\n")
		}},
		{"a release no longer offered left on the banner", func() []string {
			return shows("if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {", "if (false) {")
		}},
		{"the shown installer not checked again", func() []string {
			return shows("if (digest.empty() || actual.empty() || !update::EqualsAsciiCaseless(actual, digest)) {",
				"if (false) {")
		}},
		{"Show file without a second check", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window, "      urnw::pages::Updates().RevealInstaller();\n",
				"      urnw::UpdateChecker::RevealInExplorer(updateSnapshot_.installerPath);\n"), connect)
		}},
		{"a report banner that cannot be closed", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window, "UpdateBar().Closed(", "UpdateBar().Opening("), connect)
		}},
		{"an unsigned helper's way out leading to the helper again", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window,
				"if (updateSnapshot_.failure == Failure::Unsigned && updateSnapshot_.phase == Phase::Failed)", "if (false)"), connect)
		}},
		{"the VPN's disconnect not said", func() []string {
			return says(`message += L" The VPN disconnects while it installs; connect again once URnetwork is back.";`, "")
		}},
		{"an install shown without the VPN to reconnect", func() []string {
			return says("message = kReconnect;", "message.clear();")
		}},
		{"the helper's run shown as starting the installer", func() []string {
			return says("case Stage::Helper:", "case Stage::Idle:")
		}},
		{"the relaunch refused by its own update", func() []string {
			return checkRelaunchWaits(replace(main, "if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();", ""),
				glue, handover)
		}},
		{"the relaunch's own wait instead of the tested one", func() []string {
			return checkRelaunchWaits(main, replace(glue, "const bool ended = instance::AwaitUpdateEnd(",
				"const bool ended = AwaitTheUpdateSomehow("), handover)
		}},
		{"a relaunch that waits ten seconds", func() []string {
			return checkRelaunchWaits(main, glue, replace(handover, "kAfterUpdateBudget{120000};", "kAfterUpdateBudget{10000};"))
		}},
		{"a relaunch that waits one poll", func() []string {
			return checkRelaunchWaits(main, glue, replace(handover, "  while (updating()) {\n", "  if (updating()) {\n"))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The worker follows the feed it was started for. A channel change bumps the
// generation and drops the offer, every change a check or an apply makes to
// the snapshot is for its own generation, a check of the old feed publishes
// nothing, and an apply of it stops at the latest at the hand-off to the
// helper. The helper's report is not the feed's to drop: once the helper ran,
// what it did is shown whatever the feed is now.
func checkTrayFollowsTheFeed(checker string) []string {
	var problems []string
	unscoped := regexp.MustCompile(`\bMutate\(`)
	changed := applyDefinition(checker, "void UpdateChecker::ChannelChanged() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ChannelChanged", changed,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("++feedGeneration_;"),
		regexp.QuoteMeta("offer_ = Offer{};"),
		regexp.QuoteMeta("if (snapshot_.phase != Phase::Result) {"),
		regexp.QuoteMeta("snapshot_.offeredCode = 0;"),
		regexp.QuoteMeta("checkRequested_ = true;"),
		regexp.QuoteMeta("cv_.notify_all();"))...)
	scoped := applyDefinition(checker, "bool UpdateChecker::MutateFor(std::uint64_t generation,")
	problems = append(problems, applyOrderProblems("UpdateChecker::MutateFor", scoped,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("if (feedGeneration_ != generation) return false;"),
		regexp.QuoteMeta("fn(snapshot_);"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (applyRequested_ || manualRequested_) {"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunApply(generation, manual);"),
		regexp.QuoteMeta("if (revealRequested_) {"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunReveal(generation);"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunCheck(generation);"))...)
	if unscoped.MatchString(worker) {
		problems = append(problems, "UpdateChecker::WorkerLoop changes the snapshot without a generation")
	}
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (feedGeneration_ != generation) {"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("snapshot_.newestCode = newestCode;"),
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("snapshot_.offeredCode = offer_.code;"))...)
	if unscoped.MatchString(check) {
		problems = append(problems, "UpdateChecker::RunCheck changes the snapshot without its generation")
	}
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("MutateFor(generation,"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;"),
		regexp.QuoteMeta("if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {"),
		regexp.QuoteMeta("abandoned(dir);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("Mutate([this, &result](Snapshot& s) {"))...)
	if handOff := strings.Index(apply, "LaunchUpdateHelper("); handOff >= 0 && unscoped.MatchString(apply[:handOff]) {
		problems = append(problems,
			"UpdateChecker::RunApply changes the snapshot without its generation before the hand-off to the helper")
	}
	reveal := applyDefinition(checker, "void UpdateChecker::RunReveal(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunReveal", reveal,
		regexp.QuoteMeta("feedGeneration_ != generation"),
		regexp.QuoteMeta("MutateFor(generation, [](Snapshot& s) {"))...)
	if unscoped.MatchString(reveal) {
		problems = append(problems, "UpdateChecker::RunReveal changes the snapshot without its generation")
	}
	return problems
}

func TestUpdateApplyTrayFollowsTheFeed(t *testing.T) {
	reportProblems(t, checkTrayFollowsTheFeed(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// GitHub's answer to too many requests is honoured: a refused request's
// Retry-After or X-RateLimit-Reset holds every later request, manual or
// automatic, the cadence never schedules one before it, and the helper,
// which asks GitHub itself, is not started during it either.
func checkTrayHonoursGitHub(checker string) []string {
	var problems []string
	fetch := applyDefinition(checker, "bool FetchUrl(std::wstring const& url, const wchar_t* accept,")
	problems = append(problems, applyOrderProblems("FetchUrl", fetch,
		regexp.QuoteMeta("if (status != 200) {"),
		regexp.QuoteMeta(`headers.retryAfterSeconds = NumericHeader(request.h, L"Retry-After", 0);`),
		regexp.QuoteMeta(`headers.rateLimitResetUnixSeconds = NumericHeader(request.h, L"X-RateLimit-Reset", 0);`),
		regexp.QuoteMeta(`headers.rateLimitExhausted = NumericHeader(request.h, L"X-RateLimit-Remaining", -1) == 0;`),
		regexp.QuoteMeta("return false;"))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("held = steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("if (held) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("FetchUrl("),
		regexp.QuoteMeta("if (!fetched) {"),
		regexp.QuoteMeta(".retryAfterSeconds = headers.retryAfterSeconds,"),
		regexp.QuoteMeta(".resetUnixSeconds = headers.rateLimitResetUnixSeconds,"),
		regexp.QuoteMeta(".exhausted = headers.rateLimitExhausted,"),
		regexp.QuoteMeta(".serverUnixSeconds = headers.serverUnixSeconds};"),
		regexp.QuoteMeta("const std::int64_t wait = update::NextCheckDelaySeconds(0, limit);"),
		regexp.QuoteMeta("holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);"),
		regexp.QuoteMeta("holdUntilUnix_ = NowUnixSeconds() + wait;"),
		regexp.QuoteMeta("snapshot_.holdUntilUnix = holdUntilUnix_;"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return false;"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("held = viaHelper && steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("if (held) {"),
		regexp.QuoteMeta("fail(Failure::Held);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper("))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("RunCheck(generation);"),
		regexp.QuoteMeta("nextAuto_ = std::max(steady_clock::now() + kCheckInterval, holdUntil_);"))...)
	return problems
}

func TestUpdateApplyTrayHonoursGitHub(t *testing.T) {
	reportProblems(t, checkTrayHonoursGitHub(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// When no check has worked for 72 hours, the app says so: since the last one
// that did, or, before any has, since the first launch that tried. The connect
// screen's banner offers to try now, or says until when GitHub asked to wait,
// and the developer line says it too. Turning automatic checks off takes the
// warning down, since the app no longer keeps trying.
func checkTraySaysWhenChecksFail(checker, connect, window, developer string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const auto lastSuccess = prefs.find(kLastSuccessPrefKey);"),
		regexp.QuoteMeta("if (snapshot_.lastSuccessUnix <= 0) {"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = NowUnixSeconds();"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("if (!parsed) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, succeeded);"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = succeeded;"),
		regexp.QuoteMeta("snapshot_.checkStale = false;"),
		regexp.QuoteMeta("snapshot_.holdUntilUnix = 0;"))...)
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("s.lastCheck = CheckOutcome::Failed;"),
		regexp.QuoteMeta("s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);"))...)
	automatic := applyDefinition(checker, "void UpdateChecker::SetAutoCheckEnabled(bool on) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::SetAutoCheckEnabled", automatic,
		regexp.QuoteMeta("autoCheck_ = on;"),
		regexp.QuoteMeta("Mutate([now, on](Snapshot& s) {"),
		regexp.QuoteMeta("s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && version::kCode != 0);"))...)
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta("const bool held = snap.holdUntilUnix > nowUnix;"),
		regexp.QuoteMeta("if (snap.phase == Phase::None) {"),
		regexp.QuoteMeta("if (!snap.checkStale) {"),
		regexp.QuoteMeta("bar.IsOpen(false);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta(`L"Couldn't check for updates since " +`),
		regexp.QuoteMeta("urnw::UpdateChecker::LocalDate(snap.lastSuccessUnix)"),
		regexp.QuoteMeta(`held ? L"GitHub asked this network to wait until " + heldUntil +`),
		regexp.QuoteMeta("button.IsEnabled(snap.lastCheck != CheckOutcome::InFlight && !held);"),
		regexp.QuoteMeta("bar.IsOpen(true);"))...)
	action := applyDefinition(window, "void MainWindow::OnUpdateBannerAction() {")
	problems = append(problems, applyOrderProblems("MainWindow::OnUpdateBannerAction", action,
		regexp.QuoteMeta("case Phase::None:"),
		regexp.QuoteMeta("if (updateSnapshot_.checkStale) urnw::pages::Updates().CheckNow();"))...)
	line := applyDefinition(developer, "void DeveloperPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("DeveloperPage::ApplyUpdateCheck", line,
		regexp.QuoteMeta("if (snap.checkStale) {"),
		regexp.QuoteMeta(`L"Couldn't check for updates since " +`),
		regexp.QuoteMeta("UpdateChecker::LocalDate(snap.lastSuccessUnix);"),
		regexp.QuoteMeta("updateCheckText_.Text(hstring{text});"))...)
	return problems
}

func TestUpdateApplyTraySaysWhenChecksFail(t *testing.T) {
	reportProblems(t, checkTraySaysWhenChecksFail(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "ConnectPage.cpp")), stripComments(readAppSource(t, "MainWindow.xaml.cpp")),
		stripComments(readAppSource(t, "DeveloperPage.cpp"))))
}

// Each worker check fails on a source that drops what it pins.
func TestUpdateApplyWiringRejectsWeakerWorkers(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	developer := stripComments(readAppSource(t, "DeveloperPage.cpp"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	within := func(opener, old, replacement string) string {
		definition := applyDefinition(checker, opener)
		return replace(checker, definition, replace(definition, old, replacement))
	}
	follows := func(source string) []string { return checkTrayFollowsTheFeed(source) }
	honours := func(source string) []string { return checkTrayHonoursGitHub(source) }
	says := func(source string) []string { return checkTraySaysWhenChecksFail(source, connect, window, developer) }
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"a channel change that keeps the generation", func() []string {
			return follows(replace(checker, "    ++feedGeneration_;\n", ""))
		}},
		{"a channel change that keeps the offer", func() []string {
			return follows(within("void UpdateChecker::ChannelChanged() {", "    offer_ = Offer{};\n", ""))
		}},
		{"a channel change that keeps offering the old feed's installer", func() []string {
			return follows(within("void UpdateChecker::ChannelChanged() {", "    snapshot_.offeredCode = 0;\n", ""))
		}},
		{"a channel change that drops the helper's report", func() []string {
			return follows(replace(checker, "if (snapshot_.phase != Phase::Result) {\n      snapshot_ = Snapshot{",
				"if (true) {\n      snapshot_ = Snapshot{"))
		}},
		{"a generation-blind MutateFor", func() []string {
			return follows(replace(checker, "if (feedGeneration_ != generation) return false;", "if (false) return false;"))
		}},
		{"a check of the old feed published", func() []string {
			return follows(within("bool UpdateChecker::RunCheck(std::uint64_t generation) {",
				"if (feedGeneration_ != generation) {", "if (false) {"))
		}},
		{"a check that changes the snapshot unscoped", func() []string {
			return follows(within("bool UpdateChecker::RunCheck(std::uint64_t generation) {",
				"MutateFor(generation, [](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });",
				"Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });"))
		}},
		{"the generation read after the worker unlocks", func() []string {
			return follows(replace(checker,
				"      manualRequested_ = false;\n      const std::uint64_t generation = feedGeneration_;\n      lock.unlock();\n",
				"      manualRequested_ = false;\n      lock.unlock();\n      const std::uint64_t generation = feedGeneration_;\n"))
		}},
		{"an apply backstop for any feed", func() []string {
			return follows(within("void UpdateChecker::WorkerLoop() {", "MutateFor(generation, [](Snapshot& s) {",
				"Mutate([](Snapshot& s) {"))
		}},
		{"the helper handed a release of the old feed", func() []string {
			return follows(replace(checker,
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {",
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; }) && false) {"))
		}},
		{"a stage changed for any feed", func() []string {
			return follows(replace(checker,
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Verifying; })) {",
				"Mutate([](Snapshot& s) { s.stage = Stage::Verifying; });\n  if (false) {"))
		}},
		{"an apply of the old feed's offer", func() []string {
			return follows(replace(checker,
				"if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;",
				"if (!actionable || offer_.code == 0) return;"))
		}},
		{"the helper's report dropped with the feed", func() []string {
			return follows(replace(checker, "Mutate([this, &result](Snapshot& s) {", "MutateFor(generation, [this, &result](Snapshot& s) {"))
		}},
		{"an installer shown again for the old feed", func() []string {
			return follows(within("void UpdateChecker::RunReveal(std::uint64_t generation) {",
				"feedGeneration_ != generation || ", ""))
		}},
		{"Retry-After not read", func() []string {
			return honours(replace(checker,
				"    headers.retryAfterSeconds = NumericHeader(request.h, L\"Retry-After\", 0);\n", ""))
		}},
		{"a spent hour not noticed", func() []string {
			return honours(replace(checker,
				"NumericHeader(request.h, L\"X-RateLimit-Remaining\", -1) == 0;",
				"NumericHeader(request.h, L\"X-RateLimit-Remaining\", -1) == -2;"))
		}},
		{"a request during GitHub's hold", func() []string {
			return honours(replace(checker, "held = steady_clock::now() < holdUntil_;", "held = false;"))
		}},
		{"the helper started during GitHub's hold", func() []string {
			return honours(replace(checker, "held = viaHelper && steady_clock::now() < holdUntil_;", "held = false;"))
		}},
		{"the hold never set", func() []string {
			return honours(replace(checker, "holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);", "(void)wait;"))
		}},
		{"the hold's end never said", func() []string {
			return honours(replace(checker, "      snapshot_.holdUntilUnix = holdUntilUnix_;\n", ""))
		}},
		{"the cadence before the hold", func() []string {
			return honours(replace(checker, "nextAuto_ = std::max(steady_clock::now() + kCheckInterval, holdUntil_);",
				"nextAuto_ = steady_clock::now() + kCheckInterval;"))
		}},
		{"no baseline before the first success", func() []string {
			return says(replace(checker, "    snapshot_.lastSuccessUnix = NowUnixSeconds();\n", ""))
		}},
		{"a success never recorded", func() []string {
			return says(replace(checker, "  SaveAppPref(kLastSuccessPrefKey, succeeded);\n", ""))
		}},
		{"a success that leaves the warning up", func() []string {
			return says(replace(checker, "    snapshot_.checkStale = false;\n", ""))
		}},
		{"staleness never worked out", func() []string {
			return says(replace(checker,
				"s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);",
				"s.checkStale = false && now;"))
		}},
		{"checks turned off, and the app still says it keeps trying", func() []string {
			return says(replace(checker,
				"s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && version::kCode != 0);", "(void)now;"))
		}},
		{"the banner silent when checks fail", func() []string {
			return checkTraySaysWhenChecksFail(checker, replace(connect, "if (!snap.checkStale) {", "if (true) {"),
				window, developer)
		}},
		{"the banner promising a check during GitHub's hold", func() []string {
			return checkTraySaysWhenChecksFail(checker, replace(connect,
				`held ? L"GitHub asked this network to wait until " + heldUntil +`,
				`false ? L"GitHub asked this network to wait until " + heldUntil +`), window, developer)
		}},
		{"the banner's button that does nothing", func() []string {
			return checkTraySaysWhenChecksFail(checker, connect,
				replace(window, "if (updateSnapshot_.checkStale) urnw::pages::Updates().CheckNow();", ""), developer)
		}},
		{"the developer line silent when checks fail", func() []string {
			return checkTraySaysWhenChecksFail(checker, connect, window,
				replace(developer, "if (snap.checkStale) {", "if (false) {"))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The package closes the running app before it replaces its files, and the
// close can never fail the install:
//   - one util:CloseApplication, for URnetwork.exe, by WM_CLOSE alone, with
//     RebootPrompt="no" said out loud (WiX's default is yes) and no
//     TerminateProcess, ElevatedCloseMessage or ElevatedEndSessionMessage:
//     scheduled before InstallValidate, each of those would schedule a
//     deferred action outside the script and fail every install with 2762;
//   - only for the in-app update (UPDATE_RELAUNCH="1") and a repair or removal
//     of this package, never on a re-run of it, and only when the installed
//     URnetwork.exe is new enough to quit on WM_CLOSE (an AppSearch above
//     2026.10.5.1 where INSTALLFOLDER puts it);
//   - Wix4CloseApplications moved before InstallValidate.
//
// Every other install leaves the app to Windows Installer's Restart Manager,
// which ends an app that does not exit when asked (MSIRMSHUTDOWN=1). After an
// update the helper ran, the package starts the app again through the helper,
// unelevated, once its files are in place, and never on an uninstall or from
// the old product an upgrade removes; the helper ships as its own component.
// Uninstalling removes updates\ beside it, but not the old product's removal
// during an upgrade, whose new version installs from there; the folder's path
// is set on every install, since WixRemoveFoldersEx fails a row whose
// property is empty.
func checkInstallerCloseAndRelaunch(document xmlNode) []string {
	var problems []string
	closes := document.descendants(utilNamespace, "CloseApplication")
	if len(closes) != 1 {
		return append(problems, fmt.Sprintf("Package.wxs has %d util:CloseApplication, want one", len(closes)))
	}
	closer := closes[0]
	for name, want := range map[string]string{
		"Target":       "URnetwork.exe",
		"CloseMessage": "yes",
		"RebootPrompt": "no",
		"Condition":    `URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)`,
	} {
		if value, _ := closer.attribute(name); value != want {
			problems = append(problems, fmt.Sprintf("CloseApplication %s = %q, want %q", name, value, want))
		}
	}
	for _, deferred := range []string{"TerminateProcess", "ElevatedCloseMessage", "ElevatedEndSessionMessage",
		"PromptToContinue"} {
		if _, ok := closer.attribute(deferred); ok {
			problems = append(problems, "CloseApplication sets "+deferred+
				": before InstallValidate a deferred action fails every install with 2762")
		}
	}
	if timeout, _ := closer.attribute("Timeout"); timeout == "" || len(timeout) > 2 {
		problems = append(problems, "CloseApplication's Timeout is not a short, explicit number of seconds: "+timeout)
	}
	closable := findByID(document.descendants(wixNamespace, "Property"), "URNETWORK_APP_CLOSABLE")
	if closable == nil {
		problems = append(problems, "URNETWORK_APP_CLOSABLE is not a property")
	} else {
		if secure, _ := closable.attribute("Secure"); secure != "yes" {
			problems = append(problems, "URNETWORK_APP_CLOSABLE is not secure")
		}
		search := closable.child(wixNamespace, "DirectorySearch")
		var file *xmlNode
		if search != nil {
			file = search.child(wixNamespace, "FileSearch")
		}
		path, depth, name, minVersion := "", "", "", ""
		if search != nil {
			path, _ = search.attribute("Path")
			depth, _ = search.attribute("Depth")
		}
		if file != nil {
			name, _ = file.attribute("Name")
			minVersion, _ = file.attribute("MinVersion")
		}
		if path != "[ProgramFiles64Folder]URnetwork" || depth != "0" || name != "URnetwork.exe" ||
			minVersion != "2026.10.5.1" {
			problems = append(problems, fmt.Sprintf("URNETWORK_APP_CLOSABLE searches %q depth %q for %q above %q, "+
				"want the installed URnetwork.exe above 2026.10.5.1", path, depth, name, minVersion))
		}
	}
	if shutdown := findByID(document.descendants(wixNamespace, "Property"), "MSIRMSHUTDOWN"); shutdown == nil {
		problems = append(problems, "MSIRMSHUTDOWN is not set: Windows Installer's Restart Manager leaves an app "+
			"that does not exit running with no tray icon")
	} else if value, _ := shutdown.attribute("Value"); value != "1" {
		problems = append(problems, "MSIRMSHUTDOWN is "+value+", not 1: an app that does not exit when asked "+
			"keeps running with no tray icon after the install")
	}
	sequence := document.descendants(wixNamespace, "InstallExecuteSequence")
	if len(sequence) != 1 {
		// launch_at_startup_wiring_test.go reads the first one as the package's
		problems = append(problems, fmt.Sprintf("Package.wxs has %d InstallExecuteSequence elements, want one", len(sequence)))
	}
	var customs []*xmlNode
	for _, node := range sequence {
		customs = append(customs, node.children(wixNamespace, "Custom")...)
	}
	moved, relaunched := false, false
	for _, custom := range customs {
		action, _ := custom.attribute("Action")
		switch action {
		case "override Wix4CloseApplications_$(sys.BUILDARCHSHORT)":
			before, _ := custom.attribute("Before")
			moved = before == "InstallValidate"
		case "RelaunchAfterUpdate":
			after, _ := custom.attribute("After")
			condition, _ := custom.attribute("Condition")
			relaunched = after == "InstallFinalize" &&
				condition == `UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE`
		}
	}
	if !moved {
		problems = append(problems, "Wix4CloseApplications is not moved before InstallValidate")
	}
	if !relaunched {
		problems = append(problems, "RelaunchAfterUpdate does not run after InstallFinalize on the helper's "+
			`UPDATE_RELAUNCH = "1" only, outside uninstalls and the old product's removal`)
	}
	relaunch := findByID(document.descendants(wixNamespace, "CustomAction"), "RelaunchAfterUpdate")
	if relaunch == nil {
		problems = append(problems, "RelaunchAfterUpdate is not a custom action")
	} else {
		for name, want := range map[string]string{
			"DllEntry":    "WixUnelevatedShellExec",
			"BinaryRef":   "Wix4UtilCA_$(sys.BUILDARCHSHORT)",
			"Execute":     "immediate",
			"Impersonate": "yes",
			"Return":      "ignore",
		} {
			if value, _ := relaunch.attribute(name); value != want {
				problems = append(problems, fmt.Sprintf("RelaunchAfterUpdate %s = %q, want %q", name, value, want))
			}
		}
	}
	properties := document.descendants(wixNamespace, "Property")
	if target := findByID(properties, "WixUnelevatedShellExecTarget"); target == nil {
		problems = append(problems, "WixUnelevatedShellExecTarget is not set")
	} else if value, _ := target.attribute("Value"); value != "[#UpdaterExe]" {
		problems = append(problems, "the relaunch starts "+value+
			", not the helper: URnetwork.exe started without its after-update argument is refused by the update marker")
	}
	if property := findByID(properties, "UPDATE_RELAUNCH"); property == nil {
		problems = append(problems, "UPDATE_RELAUNCH is not declared")
	} else if secure, _ := property.attribute("Secure"); secure != "yes" {
		problems = append(problems, "UPDATE_RELAUNCH is not secure")
	}
	updater := findByID(document.descendants(wixNamespace, "Component"), "UpdaterExe")
	var updaterFile *xmlNode
	if updater != nil {
		updaterFile = updater.child(wixNamespace, "File")
	}
	if updaterFile == nil {
		problems = append(problems, "the helper has no UpdaterExe component")
	} else {
		id, _ := updaterFile.attribute("Id")
		source, _ := updaterFile.attribute("Source")
		if id != "UpdaterExe" || windowsBase(source) != "URnetworkUpdate.exe" {
			problems = append(problems, "the UpdaterExe component does not install URnetworkUpdate.exe as UpdaterExe")
		}
	}
	excluded := false
	for _, node := range document.descendants(wixNamespace, "Exclude") {
		if files, _ := node.attribute("Files"); windowsBase(files) == "URnetworkUpdate.exe" {
			excluded = true
		}
	}
	if !excluded {
		problems = append(problems, "the RuntimeFiles harvest installs URnetworkUpdate.exe a second time")
	}
	referenced := false
	if main := findByID(document.descendants(wixNamespace, "Feature"), "Main"); main != nil {
		for _, reference := range main.children(wixNamespace, "ComponentRef") {
			if id, _ := reference.attribute("Id"); id == "UpdaterExe" {
				referenced = true
			}
		}
	}
	if !referenced {
		problems = append(problems, "the Main feature does not install the helper")
	}
	var removes []*xmlNode
	if updater != nil {
		removes = updater.children(utilNamespace, "RemoveFolderEx")
	}
	if len(removes) != 1 {
		problems = append(problems, fmt.Sprintf("the UpdaterExe component has %d util:RemoveFolderEx, want one for updates\\", len(removes)))
	} else {
		on, _ := removes[0].attribute("On")
		property, _ := removes[0].attribute("Property")
		if on != "uninstall" || property != "URNETWORK_UPDATES_FOLDER" {
			problems = append(problems, fmt.Sprintf("RemoveFolderEx removes %q on %q, want URNETWORK_UPDATES_FOLDER on uninstall",
				property, on))
		}
		if condition, _ := removes[0].attribute("Condition"); condition != `REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE` {
			problems = append(problems, fmt.Sprintf("RemoveFolderEx's condition is %q, want %q: updates\\ goes on an "+
				"uninstall of this product, never under the new version an upgrade installs from it",
				condition, `REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE`))
		}
	}
	if folder := findByID(document.descendants(wixNamespace, "SetProperty"), "URNETWORK_UPDATES_FOLDER"); folder == nil {
		problems = append(problems, "URNETWORK_UPDATES_FOLDER is not set before RemoveFolderEx reads it")
	} else {
		for name, want := range map[string]string{
			"Value":    `[ProgramFiles64Folder]URnetwork\updates`,
			"Before":   "Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)",
			"Sequence": "execute",
		} {
			if value, _ := folder.attribute(name); value != want {
				problems = append(problems, fmt.Sprintf("URNETWORK_UPDATES_FOLDER's %s = %q, want %q", name, value, want))
			}
		}
		if condition, set := folder.attribute("Condition"); set {
			problems = append(problems, fmt.Sprintf("URNETWORK_UPDATES_FOLDER is set only when %q: on every other "+
				"install WixRemoveFoldersEx fails its row for the empty property", condition))
		}
	}
	return problems
}

// Each installer check fails on a package that drops what it pins.
func TestUpdateApplyInstallerRejectsWeakerPackages(t *testing.T) {
	root := repositoryRoot(t)
	data, err := os.ReadFile(filepath.Join(root, "app", "installer", "Package.wxs"))
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, tc := range []struct{ name, old, replacement string }{
		{"TerminateProcess", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" RebootPrompt="no" TerminateProcess="1" Timeout="15"`},
		{"an elevated close message", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" ElevatedCloseMessage="yes" RebootPrompt="no" Timeout="15"`},
		{"WiX's default reboot prompt", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" Timeout="15"`},
		{"a re-run of the installed package closes the app",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='URNETWORK_APP_CLOSABLE AND (UPDATE_RELAUNCH = "1" OR Installed)'`},
		{"an app without the WM_CLOSE handler closed",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='(NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE'`},
		{"the close for any install, by hand or by a tool",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='URNETWORK_APP_CLOSABLE AND (NOT Installed OR REINSTALL OR REMOVE)'`},
		{"an app that does not exit left running", `<Property Id="MSIRMSHUTDOWN" Value="1" />`,
			`<Property Id="MSIRMSHUTDOWN" Value="0" />`},
		{"updates\\ never removed", "          <util:RemoveFolderEx xmlns:util=\"http://wixtoolset.org/schemas/v4/wxs/util\"\n" +
			"                               Id=\"RemoveUpdatesFolder\" On=\"uninstall\" Property=\"URNETWORK_UPDATES_FOLDER\"\n" +
			"                               Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />\n", ""},
		{"updates\\ removed on every install", `On="uninstall" Property="URNETWORK_UPDATES_FOLDER"`,
			`On="both" Property="URNETWORK_UPDATES_FOLDER"`},
		{"updates\\ removed under the new version installing from it",
			`Condition='REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE' />`, `Condition='REMOVE~="ALL"' />`},
		{"updates\\ removed with the component, upgrades included",
			"Property=\"URNETWORK_UPDATES_FOLDER\"\n                               Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />",
			`Property="URNETWORK_UPDATES_FOLDER" />`},
		{"the folder's path set only for the removal",
			"Before=\"Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)\" Sequence=\"execute\" />",
			"Before=\"Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)\" Sequence=\"execute\"\n" +
				"                 Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />"},
		{"the gate on any app", `<FileSearch Name="URnetwork.exe" MinVersion="2026.10.5.1" />`,
			`<FileSearch Name="URnetwork.exe" />`},
		{"the close in WiX's slot", `Before="InstallValidate"`, `Before="InstallFiles"`},
		{"the relaunch elevated", `DllEntry="WixUnelevatedShellExec"`, `DllEntry="WixShellExec"`},
		{"the relaunch refused by the marker", `Value="[#UpdaterExe]"`, `Value="[#URnetworkExe]"`},
		{"a relaunch on a manual install", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`},
		{"a relaunch from the removed product", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL")'`},
		{"a failed relaunch failing the install", "Impersonate=\"yes\"\n                  Return=\"ignore\" />",
			"Impersonate=\"yes\"\n                  Return=\"check\" />"},
		{"the helper harvested twice", "        <Exclude Files=\"$(var.BinDir)\\URnetworkUpdate.exe\" />\n", ""},
		{"a second install sequence", "    <!-- Both are scheduled in the one InstallExecuteSequence below. -->\n",
			"    <InstallExecuteSequence />\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if strings.Count(source, tc.old) != 1 {
				t.Fatalf("negative control: %q is not in Package.wxs exactly once", tc.old)
			}
			path := filepath.Join(t.TempDir(), "Package.wxs")
			if err := os.WriteFile(path, []byte(strings.Replace(source, tc.old, tc.replacement, 1)), 0600); err != nil {
				t.Fatal(err)
			}
			if problems := checkInstallerCloseAndRelaunch(parseXML(t, path)); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}
