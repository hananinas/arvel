# Submission verification — 2026-10-19

Host-only rounds 2–3. No flash, monitor, erase, device contact or provider request. Builds and host suites use a network-isolated Bubblewrap namespace with private `/dev` and read-only `sdkconfig.local`. Existing untracked files are not submission changes. Round 3 also rendered embedded assets in local headless Chrome against isolated REST fixtures.

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

## Round 3 — refreshed embedded owner UI and final confirmation

Round-3 firmware at asset commit **`8fea697a7911d37a6f8eb81bc30d289d95decfd8`**, embedded app revision **`8fea697`**: **5,310,816 bytes / `0x510960`**, app slot **6,291,456 bytes / `0x600000`**, free **980,640 bytes / `0xef6a0` / 15.586853%**, IDF rounded **16%**. Binary SHA-256: **`db5ba465dd55020debdc3af411bc80958269fd5f2a33d091af68bd9487198a0c`**. This image predates the round-3 documentation commit; app-slot space is not runtime heap/audio headroom. Private firmware binaries remain local, not submission attachments.

### Regeneration and determinism finding

- A fresh `npm --prefix interface run build` exited **0**. The documented `python3 scripts/build_interface_idf.py .` then ran its offline-cached `npm install`, rebuilt the frontend and regenerated `main/WWWData.h`, exiting **0**. To exercise generation rather than the timestamp skip, only the generated header's mtime was set to epoch zero beforehand; frontend source and configuration were not edited.
- The initial incremental firmware build exited **0** but did not regenerate the header after that mtime-only change. The standalone documented script was therefore used explicitly. The subsequent firmware build visibly recompiled `web_server.cpp`, relinked and generated the refreshed image. `idf.py -D SDKCONFIG=sdkconfig.local -D SDKCONFIG_DEFAULTS=sdkconfig.defaults reconfigure` followed by `idf.py build` both exited **0**; the final 2,023-step build refreshed the cached app revision to `8fea697`.
- **The current pipeline is not byte-deterministic.** On this host Python 3.12.15 defaults `gzip.compress(..., mtime=None)` to the current time. Calling the actual `build_progmem()` twice, 1.1 seconds apart over identical static inputs, produced different headers: `60a209f70b01783a7b2e041efaa89d4ab3a72968f0d866c81463e8ef9f8be69e` versus `2c3e582f856076ce35789f74adaf001448e8853a6e92e1617150d3f9922ab36a`; the first differing line was the gzip header bytes at line 9. The installed SvelteKit also defaults `kit.version.name` to `Date.now().toString()`, and the generator uses unsorted filesystem traversal. Successful regeneration does not imply reproducible bytes. No build-system determinism repair is claimed or included in this asset/documentation round.
- Generated header SHA-256: `95f0226c40f0ecbd995f172938e59d8659d637b8c4003d8b88c0dd45ca050265`. A throwaway check decompressed all **85 assets** and compared each byte-for-byte with its freshly built static file, confirmed the fixed Home transcript focus link and negative-tabindex target, then found every compressed asset in the final firmware binary. The later final frontend build can change time-based version metadata without changing this committed header.

### Final gates

No tests, assertions, configuration, provider transport or production speech behavior were changed in this round. Only the requested final suites were rerun.

| Command | Exit | Round-3 result |
| --- | --- | --- |
| `npm --prefix interface run check` | 0 | 0 errors / 0 warnings; separate `svelte-focus-trap` package metadata warning retained |
| From `interface`: `node --test test/*.test.mjs` | 0 | 29 passed; 0 failed/cancelled/skipped/todo; no port-collision diagnostic |
| `npm --prefix interface run build` | 0 | Static production site generated |
| `CC=gcc CXX=g++ bash components/conversation_store/test/run.sh` | 0 | Checked FAT VFS; 1,000-turn archive/fault/crash/queue/heap coverage; 80-ID ledger regression with 79 exact cached results, conflict refusal and unknown-on-recovery |
| `idf.py -D SDKCONFIG=sdkconfig.local -D SDKCONFIG_DEFAULTS=sdkconfig.defaults reconfigure` | 0 | App revision refreshed to `8fea697`; cached managed dependencies used |
| `idf.py build` | 0 | Final firmware generated; size and SHA-256 above |

EIM activation again returned **1** in the read-only sandbox, while `idf.py --version` reported **ESP-IDF v5.5.3** and firmware commands passed using the documented explicit Python function. No activation error text was emitted in the captured output. The registry reported `NOTICE: Cannot establish a connection to the component registry. Skipping checks of dependency changes.` inside the isolated build. Vendor LVGL mixed-enum deprecation and libc executable-stack warnings remain; none was suppressed.

### Embedded-asset smoke and hygiene

A temporary loopback HTTP server served gzip bytes parsed from the committed header, not the loose Vite build. Only synthetic `/rest/features` and logged-out `/rest/auth/status` responses were supplied; other REST/device actions were denied. A GET of the embedded Home JavaScript returned the fixed focus-link asset. Installed Google Chrome 153 headless rendered the actual embedded Home page: owner login required, device controls disabled, the device preview explicitly unavailable and “Read recent conversation” visible. Screenshot inspected locally; no physical-device, microphone, login or model operation occurred. This was not a new keyboard/screen-reader audit.

The browser tool itself failed with `Shared browser daemon unavailable (broker start or Chromium launch failed); check omp ps for omp.browser.* daemons and /home/hanan/.omp/logs for details` (the original message wrapped `omp ps` in backticks). An explicit installed-browser attempt returned `Browser open timed out after 30000ms while launching or connecting to the browser (spawned:/usr/bin/google-chrome-stable)`. The headless retry returned `Failed to attach to google-chrome-stable on http://127.0.0.1:38047: Timed out waiting for CDP endpoint http://127.0.0.1:38047`. Direct headless Chrome screenshot capture succeeded instead. Temporary smoke server and screenshot/profile artifacts were removed after inspection.

`sdkconfig.local` and the three pre-existing untracked files retained their starting SHA-256 fingerprints. They were not staged. The files are `Arvel Buddy — ESP32 Display.html`, `THIRD_PARTY_NOTICES.md` and `scripts/VOICECHAT_DEPLOYMENT.md`. Final commit/remote equality and push output are reported in the handoff; no repository visibility or settings operation is part of this round.
