# The SDK needs one function so a Go fatal is never lost again

2026-08-10, from task #39 (the service dies ~4.5 min into a live tunnel with no
trace). Written here because the change belongs in `Ryanmello07/urnetwork-sdk`,
which this repo cannot change, and because the Windows client has already
shipped a workaround that this function would replace.

**Ask:** add `urnet_set_crash_output` to the C ABI. One function, ~15 lines of
Go, no new dependency. Signature and body are at the bottom.

## Why the client cannot do this itself

`URnetworkSdk.dll` is a Go `c-shared` build. When the Go runtime hits something
unrecoverable — an unrecovered panic on a goroutine, or a runtime `throw` such
as `concurrent map writes` — it prints the message and the goroutine stacks and
then calls `ExitProcess(2)`.

It prints them to **file descriptor 2 and nowhere else**. Not through glog, not
through any callback, not to any file the SDK controls. Under the Windows SCM a
service has no fd 2, so the entire diagnosis is written to a dead handle and the
process disappears — no C++ log line, no WER report, no Application Error 1000,
no glog `FATAL`. That is exactly the fingerprint task #39 has been stuck on, and
it is why the crash has survived several rounds of investigation with zero
stack.

Every other platform hides this. On macOS and Linux a service inherits a real
fd 2 that goes somewhere; on Android the runtime's stderr is wired to logcat.
Windows is the platform where the default is *silence*, so Windows is where the
gap shows up first — but the gap is in the ABI, not in Windows.

## What the client did instead, and why it is a workaround

`app/src/Common/Sdk.cpp` (`RedirectGoCrashOutput`) opens a file and calls
`SetStdHandle(STD_ERROR_HANDLE, h)` before anything else on the service path.
This works because `runtime.write1` (`runtime/os_windows.go`) does not cache the
stderr handle — for fd 2 it calls `GetStdHandle(STD_ERROR_HANDLE)` on **every
write** and hands the result straight to `WriteFile`.

That was verified against the shipped DLL, not assumed: with the DLL loaded and
its runtime already initialised against an inherited console stderr, repointing
`STD_ERROR_HANDLE` at a file sent every subsequent byte of runtime output to the
file. It works today.

It is still a workaround, for two reasons:

1. **It depends on an unexported implementation detail.** Nothing in Go's
   compatibility promise says `write1` must keep re-querying `GetStdHandle`. The
   day it caches, this client goes silent again and nobody will connect the two
   events.
2. **It cannot widen the traceback.** How much Go prints is decided by
   `GOTRACEBACK`, which `runtime.parsedebugvars()` reads once inside
   `schedinit()` — for a `c-shared` DLL that is `DLL_PROCESS_ATTACH`, before the
   host's `main` exists. Go also snapshots the environment block into
   `runtime.envs` at startup, so a later `SetEnvironmentVariableW` is not merely
   late, it is invisible. Measured the same way: setting `GODEBUG` after the load
   produced nothing at all. **No C host can set `GOTRACEBACK` for a DLL it
   statically imports.** The Windows client works around *that* by writing
   `GOTRACEBACK=crash` into the service key's `Environment` value at install
   time so the SCM applies it before the loader runs — which is correct, but only
   available to the one host that happens to be a Windows service.

Both problems disappear with one exported function, because Go can set both from
inside the runtime at any time.

## The function

```go
// urnet_set_crash_output sends the Go runtime's fatal output (panics, runtime
// throws) to path, and optionally widens how much it prints.
//
// path      — appended to, created if absent. Pass "" to detach and restore the
//             runtime default.
// traceback — "", "single", "all", "system", or "crash". Empty leaves the
//             current setting alone. "crash" additionally fails fast so the OS
//             can produce a core dump / WER report.
//
// Returns true on success; on failure returns false and sets *out_error.
//
//export urnet_set_crash_output
func urnet_set_crash_output(path *C.char, traceback *C.char, out_error **C.char) C.bool
```

Body, in full:

```go
if tb := C.GoString(traceback); tb != "" {
    debug.SetTraceback(tb)           // runtime/debug; panics on an unknown value,
                                     // so validate against the five names first
}
p := C.GoString(path)
if p == "" {
    return toBool(setCrashOutput(nil, out_error))
}
f, err := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0600)
if err != nil {
    setError(out_error, err)
    return false
}
if err := debug.SetCrashOutput(f, debug.CrashOptions{}); err != nil {
    f.Close()
    setError(out_error, err)
    return false
}
return true
```

Notes for whoever implements it:

- `debug.SetCrashOutput` is Go 1.23+. The DLL is built with go1.26.5, so it is
  available now.
- **Do not close the previous file** on a re-arm without checking: the runtime
  keeps a dup of the fd. Closing the `*os.File` after a successful
  `SetCrashOutput` is documented as safe (the runtime dups it), and the current
  handle should be replaced only via another `SetCrashOutput` call.
- `debug.SetTraceback` **panics** on an unrecognised string, so the five valid
  values must be checked before the call — a diagnostic entry point that can
  itself crash the process is worse than none.
- `debug.SetCrashOutput` requires the file to be a regular file, not a pipe, and
  returns an error otherwise. Surface it through `out_error` rather than
  swallowing it.
- Nothing here is Windows-specific. macOS/iOS/Android should call it too — they
  currently get the runtime's fatal output only because their hosts happen to
  have a live fd 2, which is luck rather than design.

## What the client will do once it exists

`SdkInit` gains one call, for **both** processes (service and app — the WinUI3
app is a GUI subsystem binary and has no fd 2 either, so its Go fatals are
equally invisible today):

```cpp
urnet::setCrashOutput((logDir / "go-crash.log").string(), "crash");
```

and `RedirectGoCrashOutput` plus the install-time `GOTRACEBACK` registry write
both get deleted. Roughly 120 lines of Windows-specific workaround, and its
17 selftest assertions, replaced by one line — and, more to the point, replaced
by something that does not quietly stop working if the Go runtime changes how it
writes to stderr.
