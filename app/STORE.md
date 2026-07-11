# Microsoft Store submission — certification spike (R9)

Findings from a cited research pass (2026-07-09, primary sources: Microsoft Learn
Store policies + Win32/MSI publishing docs). This is the checklist to validate
with a minimal test submission; the actual submission needs a Partner Center
account (can't be done from the build host).

## What we confirmed

1. **Win32 EXE/MSI listings are accepted.** The Store distributes traditional
   Win32 apps as EXE/MSI installers (not only MSIX). Real VPN precedent:
   ExpressVPN ships via the unpackaged-Win32 path.
   → `learn.microsoft.com/windows/apps/distribute-through-store/how-to-distribute-your-win32-app-through-microsoft-store`
   → `.../publish/publish-your-app/msi/create-app-submission`

2. **Service + kernel driver installers are allowed.** No Store policy prohibits
   an EXE/MSI installer from registering a LocalSystem service or installing a
   kernel driver. The driver just has its own signing gate (see SIGNING.md /
   attestation). Our MSI (service + wintun + split-tunnel driver) fits.

3. **No VPN-specific prohibition.** There is no VPN-specific clause. The
   applicable requirement is **Policy 10.5.1: a privacy policy is mandatory**
   (Win32 apps that access network/personal data). We must ship a privacy policy
   URL in the listing.
   → `learn.microsoft.com/windows/apps/publish/store-policies`

4. **Third-party commerce (Stripe) is allowed.** For non-game PC apps, the Store
   permits your own payment system instead of Microsoft's commerce (Policy
   10.8.x). Our Stripe-in-browser flow is admissible — this was the key open
   question and it clears.

5. **Signing:** the installer needs **Authenticode** (a standard/OV cert
   registered to the Partner Center account is sufficient — **EV not required**
   for the app installer; Microsoft does **not** re-sign EXE/MSI, unlike MSIX).
   The kernel driver is a separate track: **EV cert + Hardware Dev Center +
   attestation signing** (see SIGNING.md).
   → `.../package-and-deploy/code-signing-options`
   → `.../windows-hardware/drivers/dashboard/code-signing-attestation`

## ⚠ Correction to the plan (auto-update)

**The Store does NOT push updates to existing users on the EXE/MSI path.** Unlike
MSIX, Store-hosted EXE/MSI listings do not auto-update installed apps — the
developer ships updates by uploading a new installer AND is responsible for the
in-app update experience (Policy 10.2.9 + the "publish an update" doc).
→ `.../publish/publish-your-app/msi/publish-update-to-your-app-on-store`

This contradicts the earlier decision ("Auto-update: via the Store; no in-app
updater"). **We need our own updater after all** — a service-assisted updater
(the privileged `urnetworkd` downloads + swaps binaries, avoiding UAC), which is
the design R4/R10 already anticipated. See PLAN.md (R9 updated).

## Certification spike checklist (minimal test submission)

1. Enroll a Partner Center (individual/company) account; register the app
   installer's Authenticode/OV cert to it.
2. Build a minimal MSI (app + service, driver feature OFF) — prove the
   service-installing MSI passes the MSI app-package requirements + cert process.
   → `.../publish/publish-your-app/msi/app-package-requirements`
3. Submit with: privacy policy URL, VPN data-collection disclosure, an update
   URL/notes, and Stripe as the declared payment method. Confirm certification
   accepts the third-party commerce declaration.
4. Add the driver feature; confirm the attestation-signed `SplitTunnel.sys`
   (SIGNING.md) passes as part of the installer payload.
5. Validate the update flow: upload a v2 installer and confirm the expected
   behavior (no silent push → our in-app updater must handle it).

**Gotchas:** privacy policy is mandatory (10.5.1); EXE/MSI updates are not
pushed (build the updater); the tray-icon GUID registration is bound to the exe
path + signer, so keep the Authenticode signer identity stable across updates
(plan R5).
