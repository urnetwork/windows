// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// Every string id the app looks up must be a resource of the generated neutral
// catalog, Strings/en/Resources.resw, which carries every live key of the
// localization store. A missing id fails quietly: Loc and Format render the raw
// id (the seedphrase sheets showed "seedphrase_saved_confirm" on a button), and
// Adv, Dev and the other English-fallback lookups render their English in every
// language. A fallback whose English differs from the catalog's fails as
// quietly the other way, because the catalog's text is what shows: a key that
// means something else shows that meaning (the developer page titled its
// Recovery section "Recovery time"), and a store English that a copy change
// left behind shows the old copy (the Auto transports footer's order). The scan
// reads each call of a lookup function, or of a page helper that hands its key
// to one, and treats every narrow string literal argument spelled like a key id
// as a lookup, with the wide literal after it as its English when the function
// falls back to one; a key literal assigned to a name ending in Key
// (row.labelKey = "...";), which a lookup reads later, is one as well.
//
// Every key the code names must also be tagged windows in the store. The
// catalog carries every live key, so an untagged lookup shows its text today,
// and shows the raw id the day the platforms the key is tagged for retire it:
// the key goes dead and leaves the catalog (three Linux Earnings page strings
// were lost that way). Strings/windows-keys.txt, generated next to the
// catalogs, lists the keys tagged windows. Keys reach a lookup through calls
// the scan above cannot follow (a ternary between two keys, a function that
// returns one, a table row, a constant), so this check reads every narrow
// literal spelled like a key id that names a catalog key, not only the calls.
// The other way round, every key on that list must be looked up: a tag the app
// no longer needs keeps the key alive in every desktop catalog and in front of
// the translators for nothing.

// How a lookup function finds its resource.
type catalogLookupKind int

const (
	// the id itself
	catalogLookupPlain catalogLookupKind = iota
	// <id>.<category>, present for every plural key as <id>.other
	catalogLookupPlural
	// a key held in a name, which a plain or a plural lookup reads later
	catalogLookupEither
)

// The functions and helper lambdas that take a key id: the lookups of
// Localization.h and PageContext.h, the English-fallback variants, and the page
// helpers that pass their key to one of them.
var catalogLookupFunctionKinds = map[string]catalogLookupKind{
	"Loc":           catalogLookupPlain,
	"LocBox":        catalogLookupPlain,
	"Localized":     catalogLookupPlain,
	"Format":        catalogLookupPlain,
	"Plural":        catalogLookupPlural,
	"PluralRaw":     catalogLookupPlural,
	"PluralFormat":  catalogLookupPlural,
	"Adv":           catalogLookupPlain,
	"AdvW":          catalogLookupPlain,
	"Dev":           catalogLookupPlain,
	"DevW":          catalogLookupPlain,
	"TransportText": catalogLookupPlain,
	"Missing":       catalogLookupPlain,
	"KeyText":       catalogLookupPlain,
	"MakeAction":    catalogLookupPlain,
	"MakeTextField": catalogLookupPlain,
	"MakePicker":    catalogLookupPlain,
	"ErrorKey":      catalogLookupPlain,
	"metric":        catalogLookupPlain,
	"door":          catalogLookupPlain, // the Connect page's fold doors (ConnectPage.cpp)
	"boolRow":       catalogLookupPlain,
	"numRow":        catalogLookupPlain,
	"millisRow":     catalogLookupPlain,
	"countRow":      catalogLookupPlain,
	"head":          catalogLookupPlain,
	"fault":         catalogLookupPlain,
	"header":        catalogLookupPlain,
	"section":       catalogLookupPlain,
	"field":         catalogLookupPlain,
	"action":        catalogLookupPlain,
	"value":         catalogLookupPlain,
	"count":         catalogLookupPlain,
	"refresh":       catalogLookupPlural,

	// a refusal's line, with the screen's own line as the fallback
	// (PaymentRefusal.h)
	"PaymentRefusalTextFor": catalogLookupPlain,
}

// The lookups whose argument after the key is the English they render when the
// catalog lacks the key.
var catalogFallbackFunctionNames = map[string]bool{
	"Adv":           true,
	"AdvW":          true,
	"Dev":           true,
	"DevW":          true,
	"TransportText": true,
	"Missing":       true,
	"metric":        true,
	"boolRow":       true,
	"numRow":        true,
	"millisRow":     true,
	"countRow":      true,
	"head":          true,
	"fault":         true,
}

// Functions that take a key of something other than the catalog. None today:
// the release list's JSON is read by Common/ReleaseJson.h, outside the app.
var catalogNonLookupFunctionReasons = map[string]string{}

// One lookup a source makes, at the line of its call.
type catalogLookup struct {
	file     string
	line     int
	function string
	key      string
	kind     catalogLookupKind
	// the English the call renders when the catalog lacks the key, or ""
	english string
}

// A call argument as the scan reads it: the value of its string literals, the
// kinds of literal it held, and whether it held anything else.
type catalogArgument struct {
	value  string
	narrow bool
	wide   bool
	other  bool
}

// Spelled like a store key id.
var catalogKeyPattern = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// A key literal assigned whole to a name ending in Key.
var catalogAssignedKeyPattern = regexp.MustCompile(`\b(\w*Key)\s*=\s*"([a-z][a-z0-9_]*)"\s*;`)

// Literals the code spells like the id of a catalog key without looking that
// key up, each with what it is instead.
var catalogNonLookupLiteralReasons = map[string]string{
	"developer":  "a --preview-ui destination (MainWindow.xaml.cpp)",
	"key":        "a JSON field (SdkHost.cpp, VlessPresentation.h)",
	"other":      "a CLDR plural category (Localization.cpp)",
	"provide":    "an onboarding step id (OnboardingRouting.h)",
	"seedphrase": "a --preview-ui destination (MainWindow.xaml.cpp)",
	"widgets":    "an onboarding link step (OnboardingRouting.h)",
	"zero":       "a CLDR plural category (Localization.cpp)",
}

// Keys tagged windows that the app looks up only where the scan cannot see,
// each with where. None today: every lookup is a call or a key literal of an
// app source the scan reads.
var catalogUnscannedTaggedKeyReasons = map[string]string{}

// Keys tagged windows that the app no longer looks up, each with why, until the
// store moves windows to deprecated for them and the catalogs are regenerated.
var catalogRetiredTaggedKeyReasons = map[string]string{
	"upd_manual_install_message": "it calls a download in the user's folder verified and tells the user " +
		"to run it after a declined elevation; an installed copy now retries through the update helper, " +
		"and a portable copy's download is described as checked against GitHub's SHA-256",
}

// A narrow string literal of a source spelled like a key id, at its line.
type catalogKeyLiteral struct {
	file string
	line int
	key  string
}

// The value of the string literal whose opening quote is at `at`, escapes
// decoded, and the index after it.
func readCatalogLiteral(code string, at int) (string, int) {
	var value strings.Builder
	for at++; at < len(code) && code[at] != '"' && code[at] != '\n'; at++ {
		if code[at] != '\\' || at+1 >= len(code) {
			value.WriteByte(code[at])
			continue
		}
		at++
		switch code[at] {
		case 'n':
			value.WriteByte('\n')
		case 't':
			value.WriteByte('\t')
		case 'u', 'U':
			digitCount := 4
			if code[at] == 'U' {
				digitCount = 8
			}
			end := min(at+1+digitCount, len(code))
			if point, err := strconv.ParseUint(code[at+1:end], 16, 32); err == nil {
				value.WriteRune(rune(point))
			}
			at = end - 1
		default:
			value.WriteByte(code[at])
		}
	}
	return value.String(), at + 1
}

// The index of the quote that closes the character literal opening at `at`, or
// of the line break that ends it unclosed: a character literal never spans
// lines, so a quote misread as one cannot swallow the code after it.
func skipCatalogCharacterLiteral(code string, at int) int {
	for at++; at < len(code) && code[at] != '\'' && code[at] != '\n'; at++ {
		if code[at] == '\\' {
			at++
		}
	}
	return at
}

// The lookups in one source. Comments are blanked first, so a call in a comment
// is none; a call through a member (x.count) or the standard library
// (std::count) is not one of these functions.
func scanCatalogLookups(file string, source string) []catalogLookup {
	code := stripComments(source)
	lookups := []catalogLookup{}
	isWordByte := func(at int) bool {
		return 0 <= at && at < len(code) && isIdentifierByte(code[at])
	}
	for at := 0; at < len(code); at++ {
		switch {
		case code[at] == '"':
			_, next := readCatalogLiteral(code, at)
			at = next - 1
			continue
		case code[at] == '\'' && opensCharacterLiteral(code, at):
			at = skipCatalogCharacterLiteral(code, at)
			continue
		case !isIdentifierByte(code[at]) || isWordByte(at-1):
			continue
		}
		start := at
		for at < len(code) && isIdentifierByte(code[at]) {
			at++
		}
		name := code[start:at]
		kind, ok := catalogLookupFunctionKinds[name]
		open := at
		for open < len(code) && (code[open] == ' ' || code[open] == '\t') {
			open++
		}
		at--
		if !ok || open >= len(code) || code[open] != '(' {
			continue
		}
		// a member call, or a qualifier other than ours
		before := strings.TrimRight(code[:start], " \t\n")
		if strings.HasSuffix(before, ".") || strings.HasSuffix(before, "->") {
			continue
		}
		if strings.HasSuffix(before, "std::") {
			continue
		}
		line := strings.Count(code[:start], "\n") + 1
		// the call's top-level arguments; adjacent literals concatenate
		depth := 0
		arguments := []catalogArgument{}
		argument := catalogArgument{}
	call:
		for scan := open; scan < len(code); scan++ {
			switch character := code[scan]; {
			case character == '"':
				value, next := readCatalogLiteral(code, scan)
				if depth == 1 {
					argument.value += value
					if scan > 0 && isIdentifierByte(code[scan-1]) {
						argument.wide = true
					} else {
						argument.narrow = true
					}
				}
				scan = next - 1
			case character == '\'' && opensCharacterLiteral(code, scan):
				// a character literal argument, never a key
				scan = skipCatalogCharacterLiteral(code, scan)
				if depth == 1 {
					argument.other = true
				}
			case character == '(' || character == '[' || character == '{':
				depth++
				if depth > 1 {
					argument.other = true
				}
			case character == ')' || character == ']' || character == '}':
				depth--
				if depth == 0 {
					arguments = append(arguments, argument)
					break call
				}
			case character == ',' && depth == 1:
				arguments = append(arguments, argument)
				argument = catalogArgument{}
			case depth == 1 && character != ' ' && character != '\t' && character != '\n':
				// an identifier, a number, an operator; a wide literal's prefix
				// is read with its literal
				if !(character == 'L' && scan+1 < len(code) && code[scan+1] == '"') {
					argument.other = true
				}
			}
		}
		// a narrow literal argument spelled like a key id is a lookup, and the
		// wide literal argument after it is its English in a fallback function
		for index, key := range arguments {
			if key.other || key.wide || !key.narrow || !catalogKeyPattern.MatchString(key.value) {
				continue
			}
			english := ""
			if catalogFallbackFunctionNames[name] && index+1 < len(arguments) {
				next := arguments[index+1]
				if next.wide && !next.narrow && !next.other {
					english = next.value
				}
			}
			lookups = append(lookups, catalogLookup{
				file:     file,
				line:     line,
				function: name,
				key:      key.value,
				kind:     kind,
				english:  english,
			})
		}
	}
	for _, match := range catalogAssignedKeyPattern.FindAllStringSubmatchIndex(code, -1) {
		lookups = append(lookups, catalogLookup{
			file:     file,
			line:     strings.Count(code[:match[0]], "\n") + 1,
			function: code[match[2]:match[3]],
			key:      code[match[4]:match[5]],
			kind:     catalogLookupEither,
		})
	}
	return lookups
}

// The narrow string literals of one source spelled like a key id. Comments are
// blanked first; a wide or other prefixed literal (L"English") is English, not
// a key.
func scanCatalogKeyLiterals(file string, source string) []catalogKeyLiteral {
	code := stripComments(source)
	literals := []catalogKeyLiteral{}
	for at := 0; at < len(code); at++ {
		switch {
		case code[at] == '"':
			value, next := readCatalogLiteral(code, at)
			if (at == 0 || !isIdentifierByte(code[at-1])) && catalogKeyPattern.MatchString(value) {
				literals = append(literals, catalogKeyLiteral{
					file: file,
					line: strings.Count(code[:at], "\n") + 1,
					key:  value,
				})
			}
			at = next - 1
		case code[at] == '\'' && opensCharacterLiteral(code, at):
			at = skipCatalogCharacterLiteral(code, at)
		}
	}
	return literals
}

// Why `taggedKeys` (the keys the store tags windows) does not cover `literal`,
// or "" when it does.
func catalogTagMiss(taggedKeys map[string]bool, literal catalogKeyLiteral) string {
	if taggedKeys[literal.key] {
		return ""
	}
	return literal.file + ":" + strconv.Itoa(literal.line) + ": \"" + literal.key +
		"\" is looked up but the store does not tag it windows: add windows to the platforms of localizations/keys/" +
		literal.key + ".yaml and regenerate the catalogs"
}

// Why `key`, tagged windows, is a stale tag, or "" when `lookedUpKeys` (the
// keys the scan saw looked up), catalogUnscannedTaggedKeyReasons or
// catalogRetiredTaggedKeyReasons has it.
func catalogStaleTagMiss(lookedUpKeys map[string]bool, key string) string {
	if _, ok := catalogUnscannedTaggedKeyReasons[key]; ok || lookedUpKeys[key] {
		return ""
	}
	if _, ok := catalogRetiredTaggedKeyReasons[key]; ok {
		return ""
	}
	return `"` + key + `" is tagged windows but nothing looks it up: in localizations/keys/` + key +
		`.yaml move windows from platforms to deprecated and regenerate the catalogs, ` +
		`or list it in catalogUnscannedTaggedKeyReasons with where it is looked up`
}

// The ids of a generated key list: one per line, # comment lines skipped.
func readCatalogTaggedKeys(text string) map[string]bool {
	taggedKeys := map[string]bool{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		taggedKeys[line] = true
	}
	return taggedKeys
}

// Why the neutral catalog, `resourceValues` (each resource name's English),
// does not answer `lookup`, or "" when it does.
func catalogMiss(resourceValues map[string]string, lookup catalogLookup) string {
	where := lookup.file + ":" + strconv.Itoa(lookup.line) + ": " + lookup.function + "(\"" + lookup.key + "\")"
	resources := []string{lookup.key}
	switch lookup.kind {
	case catalogLookupPlural:
		resources = []string{lookup.key + ".other"}
	case catalogLookupEither:
		resources = append(resources, lookup.key+".other")
	}
	for _, resource := range resources {
		value, ok := resourceValues[resource]
		if !ok {
			continue
		}
		if lookup.english != "" && lookup.english != value {
			return where + " falls back to \"" + lookup.english + "\" but the catalog's English, which is what shows, is \"" +
				value + "\": align the two, or give a different meaning its own key"
		}
		return ""
	}
	return where + " is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs"
}

// The scanner's rules on synthetic sources: what is a lookup, what is its
// English, and what is not a lookup; and how a miss is reported.
func TestCatalogLookupScannerRules(t *testing.T) {
	source := `
w_.Title().Text(Loc("sample_title"));
auto text = urnw::Format("sample_format", name);
note = pages::AdvW("sample_adv",
                   L"English " L"fallback");
field(box, "sample_label", "sample_help", value);
head(2, "sample_head", L"Head");
auto n = Plural("sample_plural", count);
// Loc("comment_key")
/* Dev("block_key", L"Block") */
const char* s = "Loc(\"string_key\")";
names.count("member_call");
std::count(a, b, 'x');
const auto key = bittensor::ErrorKey(code, "sample_fallback");
Loc(dynamicKey);
Loc(std::string("built_key") + suffix);
int ms = 21'600; Adv("after_separator", L"After");
fault("sample_fault", L"Line\none — \"two\"", FaultAction::Drop);
Adv("sample_composed", L"Composed " + suffix);
Loc("sample_loc", L"Not a fallback");
row.labelKey = "sample_field";
static constexpr std::string_view kSampleKey = "sample_constant";
if (row.labelKey == "sample_compared") {}
const std::string key = "sample_prefix_" + id;
const size_t open = value.find(L'[', pos); Loc("sample_after_wide_character");
`
	kindNames := map[catalogLookupKind]string{
		catalogLookupPlain:  "plain",
		catalogLookupPlural: "plural",
		catalogLookupEither: "either",
	}
	got := []string{}
	for _, lookup := range scanCatalogLookups("synthetic.cpp", source) {
		got = append(got, lookup.function+":"+lookup.key+":"+kindNames[lookup.kind]+":"+strconv.Itoa(lookup.line)+":"+lookup.english)
	}
	want := []string{
		"Loc:sample_title:plain:2:",
		"Format:sample_format:plain:3:",
		"AdvW:sample_adv:plain:4:English fallback",
		"field:sample_label:plain:6:",
		"field:sample_help:plain:6:",
		"head:sample_head:plain:7:Head",
		"Plural:sample_plural:plural:8:",
		"ErrorKey:sample_fallback:plain:14:",
		"Adv:after_separator:plain:17:After",
		"fault:sample_fault:plain:18:Line\none — \"two\"",
		"Adv:sample_composed:plain:19:",
		"Loc:sample_loc:plain:20:",
		"Loc:sample_after_wide_character:plain:25:",
		"labelKey:sample_field:either:21:",
		"kSampleKey:sample_constant:either:22:",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("lookups:\n%s\nwant:\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	literalSource := `
Loc("sample_title");
auto fallback = Adv("sample_adv", L"wide_english");
row.labelKey = flag ? "sample_branch_one" : "sample_branch_two";
// "sample_comment"
/* "sample_block" */
const char* text = "Has spaces";
const char* upper = "SampleUpper";
const auto quote = value.find(L'"', 0);
return "sample_after_quote";
int ms = 21'600; const char* after = u8"sample_prefixed";
auto bracket = value.find(u8'['); return "sample_after_bracket";
`
	gotLiterals := []string{}
	for _, literal := range scanCatalogKeyLiterals("synthetic.cpp", literalSource) {
		gotLiterals = append(gotLiterals, literal.key+":"+strconv.Itoa(literal.line))
	}
	wantLiterals := []string{
		"sample_title:2",
		"sample_adv:3",
		"sample_branch_one:4",
		"sample_branch_two:4",
		"sample_after_quote:10",
		"sample_after_bracket:12",
	}
	if strings.Join(gotLiterals, "\n") != strings.Join(wantLiterals, "\n") {
		t.Errorf("key literals:\n%s\nwant:\n%s", strings.Join(gotLiterals, "\n"), strings.Join(wantLiterals, "\n"))
	}

	taggedKeys := readCatalogTaggedKeys(`# Generated by @urnetwork/localizations (gen/generate.mjs). DO NOT EDIT.
# The store keys tagged windows: exactly the keys the app looks up.
sample_title
sample_adv` + "\r\n")
	if len(taggedKeys) != 2 || !taggedKeys["sample_title"] || !taggedKeys["sample_adv"] {
		t.Errorf("tagged keys: %v", taggedKeys)
	}
	tagCases := []struct {
		literal catalogKeyLiteral
		miss    string
	}{
		{
			literal: catalogKeyLiteral{file: "f.cpp", line: 3, key: "sample_adv"},
			miss:    "",
		},
		{
			literal: catalogKeyLiteral{file: "f.cpp", line: 3, key: "sample_untagged"},
			miss: `f.cpp:3: "sample_untagged" is looked up but the store does not tag it windows: ` +
				`add windows to the platforms of localizations/keys/sample_untagged.yaml and regenerate the catalogs`,
		},
	}
	for _, c := range tagCases {
		if miss := catalogTagMiss(taggedKeys, c.literal); miss != c.miss {
			t.Errorf("%q: %q, want %q", c.literal.key, miss, c.miss)
		}
	}
	staleCases := []struct {
		key  string
		miss string
	}{
		{
			key:  "sample_adv",
			miss: "",
		},
		{
			key: "sample_unused",
			miss: `"sample_unused" is tagged windows but nothing looks it up: in localizations/keys/sample_unused.yaml ` +
				`move windows from platforms to deprecated and regenerate the catalogs, ` +
				`or list it in catalogUnscannedTaggedKeyReasons with where it is looked up`,
		},
	}
	for _, c := range staleCases {
		if miss := catalogStaleTagMiss(map[string]bool{"sample_adv": true}, c.key); miss != c.miss {
			t.Errorf("%q: %q, want %q", c.key, miss, c.miss)
		}
	}

	resourceValues := map[string]string{
		"sample_title":        "Title",
		"sample_plural.other": "{} items",
		"sample_held.other":   "{} held",
	}
	cases := []struct {
		lookup catalogLookup
		miss   string
	}{
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Loc", key: "sample_title", kind: catalogLookupPlain},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Adv", key: "sample_title", kind: catalogLookupPlain, english: "Title"},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Plural", key: "sample_plural", kind: catalogLookupPlural},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "messageKey", key: "sample_held", kind: catalogLookupEither},
			miss:   "",
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Adv", key: "sample_absent", kind: catalogLookupPlain, english: "Absent"},
			miss:   `f.cpp:3: Adv("sample_absent") is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs`,
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Dev", key: "sample_title", kind: catalogLookupPlain, english: "Heading"},
			miss: `f.cpp:3: Dev("sample_title") falls back to "Heading" but the catalog's English, which is what shows, is "Title": ` +
				`align the two, or give a different meaning its own key`,
		},
		{
			lookup: catalogLookup{file: "f.cpp", line: 3, function: "Loc", key: "sample_plural", kind: catalogLookupPlain},
			miss:   `f.cpp:3: Loc("sample_plural") is not in Strings/en/Resources.resw: add the key to the localization store and regenerate the catalogs`,
		},
	}
	for _, c := range cases {
		if miss := catalogMiss(resourceValues, c.lookup); miss != c.miss {
			t.Errorf("%s(%q): %q, want %q", c.lookup.function, c.lookup.key, miss, c.miss)
		}
	}
}

// Every id the app looks up is a resource of the neutral catalog, and every
// English fallback is the catalog's English.
func TestCatalogLookupEveryKeyIsInTheNeutralCatalog(t *testing.T) {
	document := parseXML(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "en", "Resources.resw"))
	resourceValues := map[string]string{}
	for _, node := range document.descendants("", "data") {
		name, ok := node.attribute("name")
		if !ok {
			continue
		}
		value := ""
		if child := node.child("", "value"); child != nil {
			value = child.Text
		}
		resourceValues[name] = value
	}
	if len(resourceValues) < 1000 {
		t.Fatalf("Strings/en/Resources.resw has %d resources", len(resourceValues))
	}
	files := appSourceFiles(t, ".cpp", ".h")
	lookupCount := 0
	fallbackCount := 0
	assignedCount := 0
	for _, file := range sortedNames(files) {
		for _, lookup := range scanCatalogLookups(file, files[file]) {
			lookupCount++
			if lookup.english != "" {
				fallbackCount++
			}
			if lookup.kind == catalogLookupEither {
				assignedCount++
			}
			if miss := catalogMiss(resourceValues, lookup); miss != "" {
				t.Error(miss)
			}
		}
	}
	if lookupCount < 800 {
		t.Errorf("the scan saw %d lookups; the app makes about a thousand", lookupCount)
	}
	if fallbackCount < 250 {
		t.Errorf("the scan saw %d English fallbacks; the app has about three hundred", fallbackCount)
	}
	if assignedCount < 20 {
		t.Errorf("the scan saw %d keys assigned to a name; the app has about thirty", assignedCount)
	}
}

// Every key the code names is tagged windows, so it stays in the catalog for as
// long as this app looks it up: each key a lookup call names, and each narrow
// literal spelled like a key id that names a catalog key, except the literals
// that are something else (catalogNonLookupLiteralReasons).
func TestCatalogLookupEveryKeyIsTaggedWindows(t *testing.T) {
	stringsDir := filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings")
	data, err := os.ReadFile(filepath.Join(stringsDir, "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	taggedKeys := readCatalogTaggedKeys(string(data))
	if len(taggedKeys) < 500 {
		t.Fatalf("Strings/windows-keys.txt lists %d keys", len(taggedKeys))
	}
	// a plural key's resources are <id>.<category>
	catalogKeys := map[string]bool{}
	document := parseXML(t, filepath.Join(stringsDir, "en", "Resources.resw"))
	for _, node := range document.descendants("", "data") {
		if name, ok := node.attribute("name"); ok {
			catalogKeys[strings.SplitN(name, ".", 2)[0]] = true
		}
	}
	files := appSourceFiles(t, ".cpp", ".h")
	misses := map[string]bool{}
	literalCount := 0
	nonLookupKeys := map[string]bool{}
	for _, file := range sortedNames(files) {
		for _, lookup := range scanCatalogLookups(file, files[file]) {
			literal := catalogKeyLiteral{file: lookup.file, line: lookup.line, key: lookup.key}
			if miss := catalogTagMiss(taggedKeys, literal); miss != "" {
				misses[miss] = true
			}
		}
		for _, literal := range scanCatalogKeyLiterals(file, files[file]) {
			if !catalogKeys[literal.key] {
				continue
			}
			if _, ok := catalogNonLookupLiteralReasons[literal.key]; ok {
				nonLookupKeys[literal.key] = true
				continue
			}
			literalCount++
			if miss := catalogTagMiss(taggedKeys, literal); miss != "" {
				misses[miss] = true
			}
		}
	}
	if literalCount < 1000 {
		t.Errorf("the scan saw %d literals naming a catalog key; the app has about seventeen hundred", literalCount)
	}
	missMessages := []string{}
	for miss := range misses {
		missMessages = append(missMessages, miss)
	}
	sort.Strings(missMessages)
	for _, miss := range missMessages {
		t.Error(miss)
	}
	// the exceptions shrink with the code
	for key, reason := range catalogNonLookupLiteralReasons {
		if !nonLookupKeys[key] {
			t.Errorf("%q (%s) is no longer a literal of the app: drop it from catalogNonLookupLiteralReasons", key, reason)
		}
	}
}

// Every key tagged windows is looked up, by a call or a key literal the scan
// sees or where catalogUnscannedTaggedKeyReasons says, so no tag outlives the
// app's use of it.
func TestCatalogLookupEveryTaggedKeyIsLookedUp(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	taggedKeys := readCatalogTaggedKeys(string(data))
	if len(taggedKeys) < 500 {
		t.Fatalf("Strings/windows-keys.txt lists %d keys", len(taggedKeys))
	}
	files := appSourceFiles(t, ".cpp", ".h")
	lookedUpKeys := map[string]bool{}
	for _, file := range sortedNames(files) {
		for _, lookup := range scanCatalogLookups(file, files[file]) {
			lookedUpKeys[lookup.key] = true
		}
		for _, literal := range scanCatalogKeyLiterals(file, files[file]) {
			if _, ok := catalogNonLookupLiteralReasons[literal.key]; !ok {
				lookedUpKeys[literal.key] = true
			}
		}
	}
	taggedKeyIds := []string{}
	for key := range taggedKeys {
		taggedKeyIds = append(taggedKeyIds, key)
	}
	sort.Strings(taggedKeyIds)
	for _, key := range taggedKeyIds {
		if miss := catalogStaleTagMiss(lookedUpKeys, key); miss != "" {
			t.Error(miss)
		}
	}
	// the exceptions shrink with the code
	for key, reason := range catalogUnscannedTaggedKeyReasons {
		if !taggedKeys[key] || lookedUpKeys[key] {
			t.Errorf("%q (%s) is no longer tagged windows, or the scan now sees it: drop it from catalogUnscannedTaggedKeyReasons", key, reason)
		}
	}
	for key, reason := range catalogRetiredTaggedKeyReasons {
		if !taggedKeys[key] || lookedUpKeys[key] {
			t.Errorf("%q (%s) is no longer tagged windows, or the app looks it up again: drop it from catalogRetiredTaggedKeyReasons", key, reason)
		}
	}
}

// A helper that takes a key and passes it to a lookup is scanned only when it
// is registered, so a new one has to be added to catalogLookupFunctionKinds (or,
// when its key is not a catalog key, to catalogNonLookupFunctionReasons).
func TestCatalogLookupHelpersAreRegistered(t *testing.T) {
	// `name = [...](... key` for a lambda, `name(... key` for a function, where
	// the parameter is a key id: a string_view or char pointer named key or
	// *Key
	definitionPattern := regexp.MustCompile(
		`(?:auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*|\b(\w+))\(([^()]*)\)\s*(?:->[^{;]*)?\{`)
	keyParameterPattern := regexp.MustCompile(`(?:std::string_view|const char\s*\*)\s*(?:key|\w+Key)\b`)
	files := appSourceFiles(t, ".cpp", ".h")
	unregistered := map[string]string{}
	for _, file := range sortedNames(files) {
		code := stripComments(files[file])
		for _, match := range definitionPattern.FindAllStringSubmatch(code, -1) {
			name := match[1] + match[2]
			if !keyParameterPattern.MatchString(match[3]) {
				continue
			}
			_, lookup := catalogLookupFunctionKinds[name]
			_, nonLookup := catalogNonLookupFunctionReasons[name]
			if !lookup && !nonLookup {
				unregistered[name] = file
			}
		}
	}
	unregisteredNames := []string{}
	for name, file := range unregistered {
		unregisteredNames = append(unregisteredNames, name+" ("+file+")")
	}
	sort.Strings(unregisteredNames)
	if len(unregisteredNames) > 0 {
		t.Errorf("key-taking functions not in catalogLookupFunctionKinds: %s", strings.Join(unregisteredNames, ", "))
	}
}
