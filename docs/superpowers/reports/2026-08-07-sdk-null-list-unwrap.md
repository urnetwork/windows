# `parseJson` throws on a `null` list — the fourth C-ABI bug Windows has found

2026-08-07, from P2 (developer / reliability screen). Written here because the
fix belongs in `Ryanmello07/sdk`, which this repo cannot change, and because
the defensive half that *did* land here needs a reason recorded next to it.

## What happens

A live `device_` crashes the app.

```
urnet: json: [json.exception.type_error.302] type must be array, but is null
```

thrown out of `detail::parseJson<ThroughputPointList>`
(`app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp:16272`), reached from
`SdkHost::ReadStats`. The chain is window `Activated` →
`ReconcileWindowPresentation` → `SetPresentationActive` → `SubscribeStats` →
`PublishStats` → `ReadStats`. `Activated` is **dispatched**, so it is not inside
`AppController::ShowWindow`'s try/catch, and the throw is unhandled: the process
dies at the moment the window first comes up.

## Why

Three layers each behave reasonably and the composition does not.

1. **Go.** `ContractViewController.GetThroughputPoints()` returns a non-nil
   `*ThroughputPointList` whose backing `values` slice is nil. `MarshalJSON`
   renders a nil slice as the document `null` — correct JSON, and what Go always
   does.
2. **`cgo/exports_gen.go`.** The generated export short-circuits on a nil
   POINTER only:

   ```go
   r0 := self_.GetThroughputPoints()
   if r0 == nil {
       return nil
   }
   return cJson(r0, "urnet_contract_view_controller_get_throughput_points")
   ```

   `r0` is not nil, so `"null"` crosses the ABI as a perfectly valid four-byte
   C string.
3. **`cgo/gen/hpp.go`.** The generated wrapper guards a NULL `char*` and nothing
   else:

   ```cpp
   char* r_c = urnet_contract_view_controller_get_throughput_points(handle());
   auto r_s = detail::takeStringOpt(r_c);
   if (!r_s) {
       return std::nullopt;      // covers a NULL pointer
   }
   return detail::parseJson<ThroughputPointList>(r_s->c_str());   // not "null"
   ```

   `nlohmann::json::parse("null").get<std::vector<T>>()` throws `type_error.302`.

Note the asymmetry that makes this a *list* problem specifically. The generator
already emits a null-tolerant `from_json` for every struct — every one of them
opens with

```cpp
if (!j.is_object()) {
    return;
}
```

so a struct-shaped getter handed `null` yields a default-constructed value and
never throws. Only the **top-level unwrap into a `std::vector` alias** goes
through nlohmann's built-in array conversion, which has no such guard. So the
blast radius is exactly the `*List` getters, and it is a one-line fix, not a
per-type audit.

## The upstream fix

**File:** `Ryanmello07/sdk`, branch `beta/custom-server`,
`cgo/gen/hpp.go` — the `parseJson` template literal at **lines 260-267**, which
is emitted verbatim into `cgo/include/urnetwork_sdk.hpp`.

```cpp
template <typename T>
inline T parseJson(const char* s) {
	try {
		nlohmann::json j = nlohmann::json::parse(s);
		/* Go marshals a nil slice as `null`. A container asked to
		   convert from `null` should be empty, not an exception:
		   `get<std::vector<T>>()` throws type_error.302, out of an
		   ordinary non-throwing-looking getter. Structs are already
		   null-tolerant (their generated from_json returns early on a
		   non-object), so this closes the last hole. */
		if (j.is_null()) {
			return T{};
		}
		return j.template get<T>();
	} catch (const std::exception& e) {
		throw Error(std::string("urnet: json: ") + e.what());
	}
}
```

Then regenerate (`cgo/gen`) and rebuild the SDK zip; `cgo/gen/abi_baseline_test.go`
covers the ABI surface and this changes none of it.

Fixing it in `exports_gen.go` instead (emit `nullptr` when the marshalled
document is `"null"`) would also work and is arguably more honest at the
boundary — but it is a change to generated code for every list getter rather
than one place, and it converts "empty list" into "no value", which the C++ side
would then have to distinguish. Prefer the wrapper.

## Why this repo could not just fix it

`app/third_party/` carries exactly one tracked file — a README. The header is a
build artifact unpacked from `URnetworkSdkWindows.zip` by
`app/tools/build-local.ps1` / the `beta-build.yml` CI job, so an edit to it is
discarded by the next unpack and would never reach CI. Go is not installed on
the machine this was found on, so the generator could not be run either.

## What landed here instead

Defensive guards at every list-shaped getter in `app/src/App/SdkHost.cpp`,
through one helper:

```cpp
template <typename Fn>
auto ReadList(std::atomic<bool>& logged, const char* what, Fn&& fn) -> decltype(fn());
```

`logged` is a per-call-site latch, because every one of these sites is a
listener callback or a poll and an unlatched log would fill the file at the
listener's rate. Sites covered:

| Site | Getter |
| --- | --- |
| `ReadStats` | `ContractViewController::getThroughputPoints` |
| `PublishThroughput` | `ContractViewController::getThroughputPoints` |
| `PublishContractRows` | `ContractDetailsViewController::getContractRows` |
| `PublishBlockActions` | `BlockActionViewController::getBlockActions` |
| `PublishSplitRules` | `Device::getBlockActionOverrides` |
| `BootstrapSession` (split-tunnel seed) | `LocalState::getBlockActionOverrides` |
| `SubscribeDrawer` (peer seed) | `PeerViewController::getPeers` |
| `ConnectedProvidePeers` | `PeerViewController::getPeers` |
| `ReadReliability` | `DeviceRemote::getExits`, `getDestinationExits` |

Five functions already had a try/catch that degrades to `LogWarn` — the offline
app-rule merge in `SubscribeDrawer`, `PushLocalOverrideAppsToDriver`,
`UpdateSplitRule`, `SetAppRule`/`RemoveAppRule` and `CurrentAppRules` — and were
left as they are; the helper matches their behaviour. Coverage across the two
mechanisms is complete: every member of the SDK header returning a `*List` alias
is either wrapped, inside one of those handlers, or not called from this app.

**These guards should stay after the upstream fix lands.** They cost nothing,
and they cover the general shape (any getter that throws for any reason no
longer takes the process down), not just this instance.

## The pattern worth naming

This is the **fourth** defect found in the C-ABI wrapper by this client, and all
four are the same kind: a boundary that every other consumer crosses through
gomobile, exercised for the first time by a compiling C++ consumer.

1. `0be71c7` — a string-returning callback has to cross as a `malloc`'d
   `char*`.
2. `744e43d` — `dupCString` was declared and never defined, plus the
   compile-only wrapper check that would have caught it.
3. `5c04ea2` — the hand-written exports were missing from the Windows `.def`.
4. this one — a `null` document unwrapped into a `std::vector`.

Android and iOS consume the same Go through gomobile and never touch
`cgo/include/urnetwork_sdk.hpp`, so nothing else in the fleet can find these.
Windows is the only compiling consumer of that header, which makes it the de
facto test suite for it — and the three earlier ones were compile-time failures
while this one is a runtime crash reachable only with a live session, which is
the first of these that review could not have caught by reading.
