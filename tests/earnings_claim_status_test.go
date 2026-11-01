// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The words an epoch's SN25α claim reads as on Earnings (support inbox 8126):
// App/EarningsSheets.cpp's ClaimStatusText, which the history table
// (WalletPage.cpp) and the claim dialog show, and the count under the
// unclaimed total. Both need WinUI, so they are read from the sources and the
// generated Strings/<locale>/Resources.resw. The SDK reports an epoch the
// vault finalized without a payout root as "not-finalized", which read "Not
// finalized"; it reads "No payout published" now. The count counts the
// claimable epochs, so it says so instead of "finalized epochs".

// status -> the catalog key ClaimStatusText returns for it
func claimStatusKeys(t *testing.T) map[string]string {
	t.Helper()
	body := functionBody(stripComments(readAppSource(t, "EarningsSheets.cpp")), "hstring ClaimStatusText(std::string const& status)")
	if body == "" {
		t.Fatal("EarningsSheets.cpp: no ClaimStatusText")
	}
	keys := map[string]string{}
	for _, match := range regexp.MustCompile(`if \(status == "([a-z-]+)"\) return Loc\("([a-z0-9_]+)"\);`).FindAllStringSubmatch(body, -1) {
		keys[match[1]] = match[2]
	}
	return keys
}

// the English an SDK claim status reads as
func claimStatusEnglish(t *testing.T, status string) string {
	t.Helper()
	key, ok := claimStatusKeys(t)[status]
	if !ok {
		t.Fatalf("ClaimStatusText has no word for %q", status)
	}
	value := reswValue(t, repositoryRoot(t), "en", key)
	if value == "" {
		t.Fatalf("en/Resources.resw has no %s", key)
	}
	return value
}

func TestClaimStatusClaimableReadsClaimable(t *testing.T) {
	if got := claimStatusEnglish(t, "claimable"); got != "Claimable" {
		t.Errorf("claimable reads %q, want Claimable", got)
	}
}

func TestClaimStatusClaimedReadsClaimed(t *testing.T) {
	if got := claimStatusEnglish(t, "claimed"); got != "Claimed" {
		t.Errorf("claimed reads %q, want Claimed", got)
	}
}

func TestClaimStatusExpiredReadsExpired(t *testing.T) {
	if got := claimStatusEnglish(t, "expired"); got != "Expired" {
		t.Errorf("expired reads %q, want Expired", got)
	}
}

func TestClaimStatusOpenReadsOpen(t *testing.T) {
	if got := claimStatusEnglish(t, "open"); got != "Open" {
		t.Errorf("open reads %q, want Open", got)
	}
}

// the vault finalized the epoch without a payout root: nothing more is
// published for it, so it never reads as not finalized
func TestClaimStatusNotFinalizedReadsNoPayoutPublished(t *testing.T) {
	if got := claimStatusEnglish(t, "not-finalized"); got != "No payout published" {
		t.Errorf("not-finalized reads %q, want No payout published", got)
	}
}

// a status this build does not know reads as itself, never as a known word
func TestClaimStatusUnknownReadsItself(t *testing.T) {
	body := functionBody(stripComments(readAppSource(t, "EarningsSheets.cpp")), "hstring ClaimStatusText(std::string const& status)")
	if !strings.HasSuffix(strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(body), "}")), "return H(status);") {
		t.Errorf("ClaimStatusText does not end with the raw status:\n%s", body)
	}
	if len(claimStatusKeys(t)) != 5 {
		t.Errorf("ClaimStatusText names %v, want the SDK's five statuses", claimStatusKeys(t))
	}
}

// The line under the unclaimed total (the claim dialog and the Earnings
// tile) counts the claimable epochs, and says so.
func TestClaimCountUnderTheUnclaimedTotalSaysClaimableEpochs(t *testing.T) {
	if got := reswValue(t, repositoryRoot(t), "en", "claim_across_epochs"); got != "Claimable epochs: {}" {
		t.Errorf("claim_across_epochs reads %q, want Claimable epochs: {}", got)
	}
	sheets := stripComments(readAppSource(t, "EarningsSheets.cpp"))
	if n := strings.Count(sheets, `urnw::Format("claim_across_epochs", static_cast<int64_t>(ClaimableCount()))`); n != 2 {
		t.Errorf("the claim dialog formats claim_across_epochs with ClaimableCount() %d times, want 2 (Build and ApplyState)", n)
	}
	count := functionBody(sheets, "size_t ClaimAlphaSheet::ClaimableCount() const")
	if !strings.Contains(count, `if (row.claim.status == "claimable" && row.state != Row::State::Confirmed) ++count;`) {
		t.Errorf("ClaimableCount does not count the claimable rows:\n%s", count)
	}
	wallet := stripComments(readAppSource(t, "WalletPage.cpp"))
	for _, want := range []string{
		`if (claim.status == "claimable") ++claimable;`,
		`urnw::Format("claim_across_epochs", static_cast<int64_t>(claimable))`,
	} {
		if !strings.Contains(wallet, want) {
			t.Errorf("WalletPage.cpp: the unclaimed tile has no %s", want)
		}
	}
	if strings.Count(sheets+wallet, `"claim_across_epochs"`) != 3 {
		t.Errorf("claim_across_epochs is formatted somewhere that is not counted here")
	}
}

func TestClaimStatusAndCountAreTranslatedInEveryLocale(t *testing.T) {
	root := repositoryRoot(t)
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"sn_status_not_finalized", "claim_across_epochs"} {
		english := reswValue(t, root, "en", name)
		for _, entry := range entries {
			if !entry.IsDir() || entry.Name() == "en" {
				continue
			}
			value := reswValue(t, root, entry.Name(), name)
			if value == "" {
				t.Errorf("%s/Resources.resw has no %s", entry.Name(), name)
			} else if value == english {
				t.Errorf("%s/Resources.resw %s is English", entry.Name(), name)
			} else if name == "claim_across_epochs" && !strings.Contains(value, "{}") {
				t.Errorf("%s/Resources.resw %s = %q has no count", entry.Name(), name, value)
			}
		}
	}
}
