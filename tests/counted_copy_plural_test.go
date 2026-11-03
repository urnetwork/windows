// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// A count in front of a noun ("3 months of Pro, free", "14 days free, then
// ...", "23 words") needs the noun's plural form for that count. These lines
// were formatted from one fixed string with Format, so Russian read "1 месяца"
// and English "1 days free". They are plural keys now: the call sites must
// select a form with Plural / PluralFormat, and the catalog carries the forms.

type countedCopy struct {
	file string
	call string
}

var countedCopies = []countedCopy{
	{"Onboarding.cpp", `Plural("offer_months_free_headline", offer.monthsFree)`},
	{"Onboarding.cpp", `Plural("offer_cta_start_trial_months_free", offer.monthsFree)`},
	{"OfferCard.cpp", `PluralFormat("offer_terms_first_year", trialDays, trialDays, first, regular)`},
	{"LoginPage.cpp", `Plural("seedphrase_word_count_warning", static_cast<int64_t>(words))`},
	// Claim now's period: the relay's 30 days today, any count tomorrow
	{"BittensorKeyViews.cpp", `Plural("sn_claim_relay_claim_now_paid", periodDays)`},
	{"BittensorKeyViews.cpp", `Plural("sn_claim_relay_claim_now_paid_each", periodDays)`},
}

func TestCountedCopySelectsAPluralForm(t *testing.T) {
	root := repositoryRoot(t)
	for _, counted := range countedCopies {
		data, err := os.ReadFile(filepath.Join(root, "app", "src", "App", counted.file))
		if err != nil {
			t.Fatal(err)
		}
		source := string(data)
		if !strings.Contains(source, counted.call) {
			t.Errorf("%s does not call %s", counted.file, counted.call)
		}
		key := counted.call[strings.Index(counted.call, `"`):]
		key = key[:strings.Index(key[1:], `"`)+2]
		if regexp.MustCompile(`(^|[^A-Za-z])Format\(` + regexp.QuoteMeta(key)).MatchString(source) {
			t.Errorf("%s still formats %s from one fixed string", counted.file, key)
		}
	}
}

func TestCountedCopyCatalogHasEachForm(t *testing.T) {
	root := repositoryRoot(t)
	want := map[string]string{
		"offer_months_free_headline.one":     "{} месяц Pro бесплатно",
		"offer_months_free_headline.few":     "{} месяца Pro бесплатно",
		"offer_months_free_headline.many":    "{} месяцев Pro бесплатно",
		"offer_terms_first_year.one":         "{0} день бесплатно, далее {1} за первый год, затем {2}/год. Отмена в любой момент.",
		"seedphrase_word_count_warning.few":  "Это {} слова — сид-фраза состоит из 12 или 24 слов",
		"sn_claim_relay_claim_now_paid.one":  "«Получить сейчас» оплачивается URnetwork, раз в {} день.",
		"sn_claim_relay_claim_now_paid.many": "«Получить сейчас» оплачивается URnetwork, раз в {} дней.",
	}
	for name, value := range want {
		if got := reswValue(t, root, "ru", name); got != value {
			t.Errorf("ru %s = %q, want %q", name, got, value)
		}
	}
	if got := reswValue(t, root, "en", "offer_terms_first_year.one"); got != "{0} day free, then {1} for your first year, then {2}/year. Cancel anytime." {
		t.Errorf("en offer_terms_first_year.one = %q", got)
	}
	for name, value := range map[string]string{
		"sn_claim_relay_claim_now_paid.one":        "Claim now is paid by URnetwork, once a day.",
		"sn_claim_relay_claim_now_paid.other":      "Claim now is paid by URnetwork, once every {} days.",
		"sn_claim_relay_claim_now_paid_each.other": "Paid by URnetwork, once every {} days for each coldkey.",
	} {
		if got := reswValue(t, root, "en", name); got != value {
			t.Errorf("en %s = %q, want %q", name, got, value)
		}
	}
}
