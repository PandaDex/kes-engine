# Crash Reporting

The engine captures crashes with [sentry-native](https://github.com/getsentry/sentry-native) and its Crashpad backend. `crashpad_handler` runs as a separate process, so stack overflows and heap corruption still produce a minidump. The reporter starts in `EngineStart`, and games need no code for it.

It works without a server. By default minidumps stay on the player's machine and nothing is uploaded.

## What happens on a crash

- The handler writes a minidump to `<user data>/crashes/reports/`. The user data folder is `%APPDATA%/<company>/<project>` on Windows and `~/.local/share/<company>/<project>` on Linux.
- The log of that run is saved under `crashes/attachments/`.
- The next run logs `The previous run crashed.`, which a game can check with `CrashReporterCrashedLastRun()`.

Every report carries the release (`<name>@<version>+<commit>`), the build configuration, and the `renderer` and `map` tags. The log's first lines give the same build identity, for example `Build: kes 1.0.0.0 (db344c6956, Release, Windows)`. A `-dirty` commit means the build had local changes.

## Uploading reports

Uploads need both of these:

1. A DSN, set at configure time: `-DKES_CRASH_DSN=https://<key>@<host>/<project>`. Any Sentry-compatible server works, including sentry.io and self-hosted Sentry.
2. The player's consent, stored as `bCrashReportUpload` in `GameSettings.ini`. It is off by default, so the game has to ask:

```cpp
EngineGetGameSettings(app).SetCrashReportUploadEnabled(true);
EngineGetGameSettings(app).SaveSettings();
```

Without consent, minidumps are still written locally. Treat this setting as a real privacy choice, because an uploaded report contains:

- the crashed process's stacks and some of its memory, which can include anything the game held, such as the pak encryption key
- the run's log, which contains file paths and so usually the player's user name

Lost GPU devices are reported as fatal events under the same rules, on Windows and GDK only. Linux builds have no event transport (it would bundle libcurl and a TLS stack into the package), so there the failure is only logged. Crash minidumps upload on every platform.

## Reading a crash

Packaging writes `<name>-<platform>-symbols-<version>.zip` next to the package. **Keep this file for every release you ship.** A minidump can only be resolved with the symbols from the exact same build. Don't ship it to players.

On Windows, unzip it next to the dump and use Visual Studio (open the `.dmp`, then Debug with Native Only) or:

```
cdb -z crash.dmp -y <symbols folder> -c ".ecxr; kc; q"
```

For uploads, send the symbols to the server with `sentry-cli debug-files upload <symbols folder>`.

On Linux, the zip has `<exe>.debug`. Use it with `minidump_stackwalk`, or with `sentry-cli` as above.

## GPU failures

Each renderer reports unrecoverable errors through `IRenderer::GetFatalError()`: a lost device, a failed submit or a lost surface. The engine then logs the reason, sends a fatal event, shows the player a message box and exits with code 1. The D3D11 reason comes from `GetDeviceRemovedReason`. The game doesn't try to recover, since that would mean reloading every GPU resource.

## Testing

`kes_crash_smoke` runs under `ctest`. It crashes a child process and checks for the minidump and for detection on the next run. It needs no GPU.

Builds with `KES_DEBUG_TOOLS` also accept these flags. Shipped packages don't.

| Flag | Effect |
| --- | --- |
| `-debug-crash` | Crash before the window opens |
| `-debug-crash-upload` | Grant upload consent for this run |
| `-debug-gpu-fault=<frame>` | Simulate a fatal GPU error on that frame (tests the exit path, not a real device loss) |

The `SENTRY_DSN` environment variable sets a DSN without rebuilding, for builds with no `KES_CRASH_DSN`.

## Build notes

- `KES_CRASH_REPORTER=OFF` removes it. The API stays and does nothing.
- `KES_SANITIZE` turns it off, because ASan needs the crash signals.
- Linux needs the libcurl and zlib development packages to build (Debian: `libcurl4-openssl-dev zlib1g-dev`). Without them CMake warns and builds without crash reporting.
- On Linux, `crashpad_handler` loads libcurl at runtime and only for uploading. Without libcurl on the player's machine, minidumps are still written but not uploaded.
- `crashpad_handler` and, on Windows, `crashpad_wer.dll` must sit next to the executable. `kes_add_game` and `kes_package` put them there. If the handler is missing, the game logs it and runs without crash reporting.
