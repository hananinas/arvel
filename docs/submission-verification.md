# Submission verification — 2026-10-19

Host-only round 2. No flash, monitor, erase, device contact, provider request or registry query. Builds and host suites use a network-isolated Bubblewrap namespace with private `/dev` and read-only `sdkconfig.local`. Existing untracked files are not submission changes.

## Diagnostics

- The transcript remains a `role="log"` live region. A “Read recent conversation” anchor focuses its `tabindex="-1"` target, following the app's existing skip-link pattern. Svelte check reports **0 errors / 0 warnings** (previously one accessibility warning). Actual Chromium on the static site with isolated REST responses: keyboard Enter moved focus to the log, PageDown moved `scrollTop` from 0 to 227, and the visible focus outline measured 3 px. The smoke inserted local overflow text; it did not read a device transcript or exercise a screen reader.
- **Retained:** `svelte-focus-trap@1.2.0` declares `svelte: src/index.js` but no `exports` map. The installed Vite Svelte plugin warns by inspecting package metadata. The local npm cache contains only the 1.2.0 tarball; a newer release was not verified because network access is prohibited. No supported local metadata override was found. Patching `node_modules` would be non-reproducible and is prohibited; disabling the warning would only hide it. No package version, lockfile or dependency files were changed.
- Both Vite middleware fixtures previously set `hmr: false`, but Vite 5.4.19 still creates a WebSocket server and falls back to port 24678. Each now supplies its own Node HTTP server listening on port 0 at 127.0.0.1 and closes it after Vite. Five concurrent-file repetitions passed **27/27 each**, with no `WebSocket server error: Port is already in use`. Assertions and test counts were unchanged.

## Interface gates

All exited 0 after the fixture changes:

| Command | Result |
| --- | --- |
| `npm --offline --no-audit --no-fund --prefix interface ci` | 255 packages installed from local cache; no registry access |
| `npm --prefix interface run check` | 0 errors / 0 warnings; separate dependency metadata warning retained |
| From `interface`: `node --test test/*.test.mjs` | 29 passed, 0 failed/cancelled/skipped/todo |
| `npm --prefix interface run build` | Static production site generated |

The offline flags constrain dependency installation for this verification; ordinary `npm --prefix interface ci` remains the fresh-machine instruction. These results do not establish physical-board behavior or live inference.

## README truth pass and host gates

All commands below exited 0 with the installed/cached dependencies. Versions: Node 22.23.3 and npm 10.9.9 (round-1 environment); this pass's explicit firmware checks reported ESP-IDF v5.5.3 and `rustc +esp` 1.97.0-nightly.

| Command | Round-2 result |
| --- | --- |
| `npm --prefix interface ci` | 255 packages; `npm_config_offline=true`, audit/fund disabled |
| `test -e sdkconfig.local` (README's create-if-absent guard) | Existing file detected; no copy or configuration write |
| `bash components/agent_runtime/test/run.sh` | 5 Rust tests and C++/Rust integration executable passed |
| `bash components/ai_chat_bsp/test/run.sh` | Desk behavior, spoken-end boundaries and settings behavior executables passed |
| `bash components/codex_auth/test/run.sh` | 37 scenarios + 37 fresh-process reload probes passed |
| `CC=gcc bash components/web_auth/test/run.sh` | 6 scenarios passed |
| `cmake -S components/speech_tts -B /tmp/arvel-speech-host` | Host configuration generated |
| `cmake --build /tmp/arvel-speech-host --target speech_live_protocol_test speech_tts_smoke -j 4` | Both targets built |
| `/tmp/arvel-speech-host/speech_live_protocol_test` | 30 protocol checks passed |
| `/tmp/arvel-speech-host/speech_tts_smoke /tmp/arvel-speech.pcm` | 84,123 frames, 63,415 nonzero; hash `873f95beff37c051`; synthesis/failure/recovery checks passed |
| `idf.py -D SDKCONFIG=sdkconfig.local -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build` | Full 2,024-step build passed using EIM environment and explicit Python function |
| `idf.py build` | Incremental build passed with the configured private SDKCONFIG |
| From `interface`: `UNRAVEL_DEVICE_URL=http://127.0.0.1:9 npm run dev -- --host 127.0.0.1` | Startup succeeded; loopback GET `/` returned HTTP 200; no proxy/device call |

Round-2 image at diagnostic commit `255fa83`: **5,311,968 bytes / `0x510de0`**, slot **6,291,456 bytes**, free **979,488 bytes / 15.568542%**, IDF rounded **16%**. SHA-256: `1ed1c7427e0473fefb59a939ecd83f10a2cb6db08cbd5fcaf4d3e68951e9efc4`. Later docs and host-only regression commits are not represented by that embedded build revision.

The conventional `export.sh` failure is a round-1 finding, not re-run here. EIM activation sets the correct environment and an interactive alias. This pass used a Bash `idf.py` function invoking `"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"`. In the read-only sandbox EIM's optional IDE-selection sync made the activation script return 1; the version check and both builds succeeded. No installer layout was changed.

Initial sandbox mount ordering was incorrect: it produced `/dev/null: Permission denied`, `Error running link command: permission denied` in speech CMake (exit 1), and `ninja: fatal: posix_spawn: Permission denied` in firmware (IDF exit 2). Mounting the read-only root first, then private `/dev` with only the host null device bound, fixed the harness. A conditional function definition was also skipped after EIM's nonzero optional sync, producing `bash: line 1: idf.py: command not found` (exit 127); defining the function after activation without chaining on that status fixed automation. These were host/harness corrections, not production changes or weakened gates.

The first browser smoke supplied HTTP 401 for the boot-time `/rest/features` request and hit `Error: Device information is unavailable. Check your connection and reload.` Supplying the correct isolated features response allowed the actual keyboard/scroll smoke to pass. This was a fixture setup error, not device evidence.

Not executed: internet-dependent toolchain provisioning (`espup install`), the already-reported failing stock activation, interactive `menuconfig`, flashing/monitoring, real device login/proxy, providers or cloud deployment. Board-revision and flashing guidance is unchanged. Firmware compilation still emits vendor LVGL mixed-enum deprecation and libc `missing .note.GNU-stack section implies executable stack` warnings.

## Greater-than-64 delegation ledger regression

`components/codex_auth/test` covers account authorization, not delegation deduplication. `components/ai_chat_bsp/test` covers desk tools/settings without the durable ledger. The existing real-SQLite ledger fixture is `components/conversation_store/test/behavior.cpp`; one new scenario, `delegation_ledger_over_64`, runs there.

It admits 80 distinct delegation `run_id`s and provider call IDs in one conversation/session, executes the real read-only `search_history` operation, commits 79 results and leaves the last read without terminal acknowledgment. After changing the searchable archive, it checks every completed ID still returns its exact original result rather than authorizing execution; conflicting arguments are rejected for all 80 IDs. It checks both before and after storage shutdown/reopen, including `inflight` becoming `unknown` for the unacknowledged read. This exercises the ledger admission boundary, not the complete Codex transport, model loop or C++ registered-tool callback.

`CC=gcc CXX=g++ bash components/conversation_store/test/run.sh` exited **0**: checked FAT VFS, the existing 1,000-turn archive/fault/crash/queue/heap checks and the new 80-ID scenario passed. The default Nix compiler initially failed with `fatal error: 'openssl/sha.h' file not found` (exit 1); selecting installed system GCC/G++ resolved the host include-path mismatch without repository build changes.

Production `speech_live.cpp` is unchanged: its `delegations[64][129]` array and `delegation_count == 64` admission check still fail at `delegation_limit` on a 65th distinct live delegation. That is a source-observed current limitation, not a live failure reproduced by this host regression. Passing the durable ledger at 80 IDs does **not** establish more than 64 delegations in a real Codex voice call. Removal remains deferred until a physical baseline exists.
