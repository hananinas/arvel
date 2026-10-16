# Arvel firmware changelog

## Codex-first app correctness — 2026-10-14

- Applied the owner's current priority: Codex now, NVIDIA testing later. Retained the native adapter without automatic switching/deployment; the saved plan distinguishes future hackathon gates from current app work. No ASR adapter was added.
- Corrected Codex account/setup guidance to select the conversation transport, not standalone read-aloud. Home explains the separate text-backend requirement for delegated actions/search without inventing STT or account eligibility.
- Repaired nullable MQTT/NTP/AP/system forms and loading/unavailable states, native submit typing and owner focus references. Removed unsafe modal component casts; confirmation content permits only simple attribute-free formatting. Captured the chosen upload file, handled empty selection, made missing OTA progress indeterminate and cleaned up progress/reload/poll/chart lifetimes.
- Moved LittleFS normalization to Rollup's per-output hook, supporting string/function naming and hash-only templates without disrupting output paths. Added two real generated-asset regressions for the 32-byte basename boundary. Fixed socket timer types, final-listener unsubscribe, old-peer closure and reconnect subscription recovery.
- `npm run check` reports **0 errors/1 warning**, down from 51 errors/seven warnings; the focusable transcript log keeps keyboard scrolling and a visible focus outline. **29 behavioral tests** pass. Existing `svelte-focus-trap` exports and vendor libc executable-stack warnings remain.
- Actual Chromium with isolated REST responses exercised Codex selection/no STT/native setup, one busy Stop, login error/success focus, 320-pixel reflow, loaded/unavailable admin pages, one AP settings fetch/mount and cancelled restart/reset/sleep/upload without POSTs. Actual Chart instances handled delayed PSRAM capacity and destroyed on navigation; firmware telemetry flags remain disabled. Real dialogs rejected executable HTML, rendered indeterminate/valid OTA progress and cancelled stale reload beyond its deadline. Real local WebSocket peers exercised unsubscribe/reinitialization/reconnect and preserved values/state transitions.
- Final embedded ESP-IDF 5.5.3 image: **`0x510d60` bytes**, **`0xef2a0` bytes (16%) free** per 6 MiB slot. Not flashed; no cloud spend, microphone capture or household action. Codex's reported longevity failure, 64-delegation ceiling and Opus/fingerprint behavior remain unchanged pending the original physical baseline. The previous candidate's verification below is historical, not additional current physical evidence.

## Native audio, bounded SD memory and safe direct Niko — 2026-10-14

- Added the released NVIDIA Labs VoiceChat 11B native PCM/WSS adapter and explicit saved conversation mode. Verified TLS/bearer ingress, fixed-state 16 kHz stereo ↔ 24 kHz mono conversion, bounded audio/event/transcript queues, final-only archival and generation-fenced worker dispatch. Structured calls execute through the same registry without a second text model/STT/TTS service. Native typed conversation/arbitrary read-aloud refuse; no hidden fallback or automatic microphone restart. Published deployment pins remain GPU-unverified.
- Added provider-scoped models.dev discovery/capability filtering and explicit custom/self-hosted native configuration. Unknown capabilities/pricing remain unknown; catalog selection never claims runtime support. Native health checks use only the saved authenticated deployment and do not capture audio or establish exercised inference.
- Migrated the C++/Rust tool contract to actual conversation/provider-session/run/call identity, origin, generation, deadline and joined cancellation. Synchronous policy ACKs control durable admission/terminal persistence; observers do not. Exact completed outcomes reuse, conflicts refuse and uncertain outcomes stay unknown. Existing twelve tools plus history and three Niko tools fit sixteen. Codex transport, Opus/fingerprint behavior and its 64-delegation ceiling are unchanged pending the original physical failure baseline.
- Added pinned unmodified SQLite 3.50.4 with one owner, bounded queue/PSRAM allocation/cache, checked FAT VFS and `PERSIST`/`FULL` journaling. Native checkpoints archive before complete-group eviction at 24 KiB/48-message soft limits; unknown outcomes stay protected. Startup recovers running tools as unknown and restores bounded conversation context, never microphone/actions. Fixed actual 1,000-turn deletion allocation failure using bounded child deletions within one transaction, distinguished in-file short reads from legitimate EOF, and allowed only absent `-wal`/`-shm` probes while refusing existing unsupported journals without deleting them.
- Added owner history pages/search/new/resume/close/logical deletion and one streamed JSONL export over messages, session boundaries, tools, sources and evidence. Pages remain 20 rows/48 KiB; encoded UTF-8 filters use bounded 2304-byte HTTP URI capacity. Sources retain actual call/session/conversation identity and optional request/UTC provenance; absent values are not invented. Source links validate schemes and suppress referrer; retrieved text is untrusted evidence.
- Added verified direct Niko Hobby MQTT TLS 8884 with write-only username/JWT/CA, explicit controller identity, bounded read-only discovery and empty default allowlist. Writes require official supported dimmer/property metadata, one enabled UUID and absolute desired properties through the durable worker. Relays/sockets/scenes remain read-only. Post-publication evidence is admitted only after publish returns and within the same connection epoch; disconnect invalidates immediately, read-only reconnect refreshes subscriptions/discovery and never replays controls. Publication/matching observation is not physical or causal proof.
- Extended Home/Settings/native projections with actual tool states/errors/sources, storage/conversation/provider identity, readiness and explicit privacy limits. Stop remains independent of busy work. Queued owner text/voice/memory/Niko/health jobs privately retain/recheck owner authority; valid logout/recovery cancels shared native activity. Tokens never enter model context/archive/UI. This is not an immediate-cookie-expiry guarantee for an established live session.
- Fixed native tool detail prioritization so long target/call identifiers cannot erase domain errors, and constrained fitted labels to one line so metadata/model names cannot wrap outside their rows. Local voice/rate preferences are labeled as local, not provider speech controls. An actual UI/LVGL public-API regression failed on the original detail formatter and passed after the fix; host-rendered six-state, uncertain-result, busy-End, readiness and missing-archive surfaces were exercised. This is logical framebuffer evidence, not physical-panel acceptance.
- Exercised actual SDK/Rust callback/checkpoint/cancellation regressions; desk/settings/search boundary tests; 1,000-turn real SQLite/VFS archive/ledger/recovery/queue/heap/fault checks; Niko protocol tests and an actual-production-actor host smoke for blocked publication, disconnect/queued-event races, genuinely later matching observations and fresh rediscovery before control. Actor dependencies/admission were explicit fixtures, not real TLS/controller/household evidence. Native protocol/media/WebSocket host checks and six `web_auth` core scenarios pass.
- Actual Chromium used isolated REST responses and real host-store export data: all six outcomes, only succeeded marked successful, Stop while busy, 45 messages across seven export pages/five kinds, health without microphone, relay refusal, cancelled then one confirmed exact dimmer request, controller-hop secret/allowlist clearing. At 320 px, document width stayed 320 with no duplicate IDs/page errors. No settings auto-save, browser/device microphone or real controller request occurred. Frontend 27 behavioral checks and production build pass; 51 inherited type errors/seven existing warnings remain.
- Owner approved two 6 MiB OTA slots without factory, preserving NVS/key/SPIFFS addresses. New OTA metadata is at `0xc10000`; a full matching bootloader/table/initial-metadata/app migration is required before app-only updates. Final ESP-IDF 5.5.3 image builds at **`0x510cd0` bytes**, with **`0xef330` bytes (16%) free**. No flash/erase/reset was performed.
- Added exact root AGPL-3.0 integration license, retained modified-runtime notices and full actual font licenses/provenance. Restricted Espressif/GPL-AGPL compatibility, NVIDIA evaluation/public-demo permission and Arvel artwork/branding rights remain unresolved: no license-cleared combined distribution/public-demo claim. Setup/audio/API/migration/privacy documentation now distinguishes implementation/host fixtures from real acceptance.
- Remaining gates: authorized Nebius access/quota/spend and qualifying GPU/audio/tools; authenticated paid-search evidence; approved full-layout installation and physical HTTP/SD fault/restart/audio/60-minute/Codex-baseline checks; permitted actual household dimmer change; public-use/distribution rights and genuine submission evidence. Cameras remain deferred. No paid inference, raw room audio, credentials or household footage was collected.

## Native deployment gate and redacted voice diagnostics — 2026-10-14

- Added a bounded, owner-input-gated NVIDIA Labs VoiceChat deployment runner, native WAV/tool smoke client and numeric-only serial baseline collector under `scripts/`. Checkpoint and container licensing are recorded separately; published image/model pins are not described as GPU-tested, and public permission remains unverified.
- Unverified bootstrap session limits must stay explicitly unknown. Verified rollover requires measured boundaries; a first deployment does not require evidence that can only be collected after it starts. TLS/bearer ingress publishes only the exact realtime/readiness routes; internal serving ports remain private. Finite service lifetime does not stop Nebius VM billing.
- Added sampled queue/heap/stack metrics to Codex live without changing its delegation ceiling or claiming to repair the reported disconnect/watchdog. Ordinary logs, credentials, room audio and entire transcripts are excluded from collected evidence.
- Exercised real CLI validation/rejection paths, synthetic serial collection, native WAV validation, the host WebSocket protocol check and nonzero host PCM generation. ESP-IDF diagnostic firmware built at `0x475720` bytes with 6% slot headroom; not flashed. No paid inference, physical long-call baseline or home-control command ran. Those external acceptance gates remain open.

## Native tool pages from the HTML mockup — 2026-10-12

- Fixed a frozen screen caused by the tool pages: `render_desk()` used 8,048 B of stack on the 4,000 B LVGL task (measured with `-fstack-usage`). Snapshot buffers now live in one PSRAM block (render 896 B total, `refresh_page` 1,024 → 224 B). Pages are built on first open instead of at boot (~30 KB of widgets measured on host), the notes table and decoded icons/bitmaps go to PSRAM, and the bell animation no longer sits in the scene-truncated track list. A boot log line reports LVGL stack headroom and internal heap; a warning repeats every 10 s if headroom < 768 B or internal heap < 24 KB.
- Added the mockup's screens 10–14 to the native LVGL UI (`components/ui_bsp/buddy_ui.c`): Pomodoro/plain timer with progress ring, round dots and Pause/Resume, +5 min, Cancel; timer done with swinging bell, Start break / Start round / Done and Snooze 5 min; scrolling to-do and notes list with checkboxes and Today/weekday tags; calculator with expression, answer and lb/oz/g-style unit picker; weather with current conditions, four hourly forecasts and high/low/feels-like/rain line. Idle now shows the mockup's Focus 25m / To-dos / Convert / Weather shortcuts in place of the "Last:" chip.
- A page opens as soon as its tool call returns (mid-turn, also in live voice) and stays up through the spoken/silent reply until closed, a panel is opened or the next request starts; also from the idle shortcuts and, for timer done, on expiry; page controls run as native desk-tool requests without a model turn. Timer, notes, weather and a new calculation snapshot (`desk_buddy_calc_snapshot`) are pushed from `ai_chat_bsp.cpp`. Tool rows use the new timer/notes/calc/weather icons with specific targets ("25 min", "add", "kg → lb", the location).
- `scripts/generate_arvel_assets.py` now also emits the four tool icons, the WxIcon weather icons, close/bell/checkbox glyphs, the pages' font styles (UI-only styles subset to their glyphs; synthetic italic for notes) and → in the mono fonts. Re-run `idf.py reconfigure` once so the font glob sees the new files.
- Verified with a host LVGL renderer against Chrome renders of the mockup, light and dark scenes, ASan/UBSan clean apart from the known LVGL mask-callback warnings; desk behaviour test extended for calculation snapshots and passes. Firmware builds at `0x474f70` bytes with 6% app-slot headroom; not flashed.

## Search credentials in browser Settings / localhost — 2026-10-12

- Added **Settings → Search tools** with provider selection, enable/disable, separate masked Tavily/Parallel keys, saved-key indicators and independent clear controls. Save/reload/status/draft handling uses the existing owner-authenticated settings endpoint. Keys are erased from drafts and request objects after submission, on logout and on unmount; status refresh preserves unsaved edits. Other desk tools explain their local/keyless operation; existing chat/transcription/Codex setup remains.
- Added typed search patches and provider/key/clear validation, including the 1,024-byte ASCII boundary. Five settings regressions pass; the production UI has real label/description relationships, keyboard focus and 320-pixel reflow without horizontal scrolling or duplicate IDs.
- Replaced the stale development target with configurable `UNRAVEL_DEVICE_URL` (default current board `http://192.168.1.22`). Only matching loopback browser origins are translated to the upstream device origin; owner sessions and mandatory-origin checks remain on the board. Actual device proxy smoke observed ordinary status, same-origin validation, foreign/missing-origin `403`, and unauthenticated Settings `401`, without changing credentials.
- Actual Chromium/proxy fixture flow covered login, both key saves, provider switching/reload, independent clearing, draft-safe status, failed-save key clearing, logout/relogin and disabling search while retaining keys. Real provider credentials and live device NVS writes were not used.
- Built embedded UI/firmware `0x4505f0` bytes with 9% slot headroom; not flashed. Full type checking still reports 51 errors/seven warnings in unchanged template/plugin files; no diagnostics in the changed settings/config files. Native UI/HTML mockup untouched.

## Settings response contract repair — 2026-10-12

- Reproduced the reported “unexpected settings response” in the actual Settings page: the firmware's public `search` metadata was rejected by the browser's old six-field allowlist. Removing only `search` made the old validator accept the same response.
- Added the required typed search metadata and strict provider/boolean/field validation in `interface/src/lib/buddySettings.ts`. Unknown fields and returned credentials still fail validation; no fallback/default shim, storage change or UI layout/native/mockup edit.
- Three permanent validator regressions pass after failing before the fix. Actual Chromium replay verified loading, a successful profile save, both provider options, and non-secret draft retention/submitted-key clearing after an invalid save response. Browser verification used isolated REST responses, not the device's live owner session.
- Rebuilt the browser and generated embedded assets into `0x44fef0`-byte firmware with 9% app-slot headroom. This corrected image is not flashed. `npm run check` reports 51 errors/seven warnings in unchanged template/plugin files; none are in the changed settings module.

## Desk utilities and selectable Tavily / Parallel search — 2026-10-12

- Added real `search_web`, `manage_timer`, `manage_notes`, `calculate`, `convert_units`, `get_weather` alongside the six board tools. Raised the C/Rust registry cap to sixteen; retained one active worker/sequential tool execution and zero-tool legacy HTTP isolation.
- Tavily is the default selected provider, with Parallel.ai retained as a settings option and separate write-only credentials in versioned `buddy/search` NVS. Official APIs use Tavily `/search` Bearer auth / fast snippets and Parallel `/v1/search` `x-api-key` / bounded advanced excerpts. Fixed verified destinations, eight-second budget, four-source cap, UTF-8-safe snippets, no raw-page/generated-answer fetch, redirects or credential output.
- Implemented monotonic focus/timer pause/resume/extension/cancel, explicit break/next-round/snooze transitions and stale-safe expiry acknowledgements. Added bounded persistent notes/tasks with stable IDs, editing/due dates/completion/removal; publish only after successful storage and do not erase unreadable data. Flash I/O releases the snapshot mutex, keeping timer/UI readers independent of storage waits.
- Added bounded on-device arithmetic/unit conversions and keyless requested-location Open-Meteo current/four-hour weather, including actual validity/fetch timestamps and station offset; failed fetches do not make cached data look fresh.
- Added native queued utility submission and thread-safe POD interfaces for the user's 640×172 utility UI; no HTML/native UI edits, LLM/history/microphone action for direct controls, permanent added worker or host shell/file/Git/MCP authority. Queued flash/HTTPS uses one temporary joined 12 KiB internal stack; bounded state/body storage uses PSRAM.
- Real-core ASan/UBSan scenarios pass for timer/state/storage/math/search/weather boundaries. Existing actual C++/Rust allocation-failure and Codex authentication suites pass. Registered-tool C++/Rust smoke exercised calculator-to-conversion chaining, structured disabled-search errors and embedded-NUL rejection before a real temporary-file write. Actual settings/crypto smoke verified legacy-record compatibility, independent provider-key persistence/clearing, public-key redaction and secret-buffer wiping on failed reads.
- Real-core host HTTPS smoke fetched Copenhagen weather/four future hours and observed both providers' `401` handling with deliberately invalid keys; authenticated results require owner keys and are unverified. Integrated application `0x44fc70` bytes builds with 9% slot headroom; this feature image is not flashed or physically UI/voice-tested. Earlier active-call watchdog limitation remains unresolved.

## Dual-core rendering and agent workspace lifetimes — 2026-10-12

- Moved the existing 4,000-byte, priority-4 LVGL task from core 0 to core 1, away from the core-0 Wi-Fi driver. No additional production thread, stack, framebuffer or DMA buffer. Network/audio task affinity and priorities remain unchanged; codec stacks and queue bounds were not reduced.
- Controlled board comparison fixed the presentation to the same scene without saving preferences. A temporary core-0 task generated nonzero PCM and used the real production-configured Opus encoder/decoder for 1,000 paced 20 ms frames; it never read/wrote I²S, opened microphone capture or contacted a provider. Stable codec-active windows measured weighted mean rotation/transfer at 29.168 → 21.170 ms (27.4% lower), drawing-handler time at 54.589 → 39.968 ms (26.8% lower), and largest handler at 100.664 → 64.303 ms (36.1% lower). Startup scene decoding and mixed windows were excluded.
- Both placements completed all codec frames with zero missed deadlines and matching decoded hash `8d607b8a`. Split-core codec work averaged 6.826 → 7.327 ms (7.3% longer); maximum work was 12.827 → 13.083 ms. This tradeoff supports isolating rendering without adding threads, but does not prove sustained real network/duplex microphone latency or physical touchscreen feel.
- Replaced each Agent's retained 128 KiB RPC buffer with per-run ownership, freeing it immediately after response parsing and on error. Native/legacy still share their existing serial worker. Freed the request cJSON tree before Rust/model/tool callbacks and released session string capacity on reset. Existing response/history/prompt limits, conversation isolation, non-copyable/movable ownership and joined callback lifetimes are preserved.
- Matching steady-state scene profiles measured free PSRAM at 7,068,272 → 7,330,404 bytes, approximately 256 KiB recovered. Two consecutive real native `get_device_status` runs completed through the actual C++/Rust loop with four scripted provider callbacks; lowest sampled callback internal heap improved from 64,407 → 76,699 bytes and largest block from 20,480 → 31,744 bytes. The provider was scripted, not a real authenticated Codex session. Explicit reset recovered another 3,332 internal bytes in this small probe session.
- Added the real C++/Rust RPC allocation-failure regression: failed workspace allocation preserves history, retry continues without the rejected prompt, and reset starts a fresh conversation. It failed against retained-buffer behavior and passed after the per-run change under ASan/UBSan. The existing callback-cancellation/context-lifetime regression also passed.
- The integrated RAM/core candidate repeated the codec probe successfully: steady-load weighted mean transfer/drawing-handler time was 21.146/40.236 ms, largest handler 75.409 ms; codec work averaged 7.328 ms, maximum 12.920 ms, with the same decoded hash and no missed deadlines. This is the final integrated profiling result rather than a promise that the isolated comparison's smaller maximum will hold on every run.
- Removed all temporary source hooks, forced benchmark scene and automatic scripted tool/codec tasks. Clean application image `0x448a80` bytes built with 10% application-slot headroom. The first application-only flash attempt was blocked by USB disconnection; after reconnection the image was written at `0x10000` with hash verification, preserving NVS, partition table, Wi-Fi and accounts. Boot reported ELF hash prefix `216e2fb69b414b34`, successful LCD/codec/native-agent/web startup and internal free/largest heap of 132,727/59,392 bytes with 7,329,784 PSRAM bytes free. A 55-second actual ESP-IDF monitor observation exited cleanly; Home returned HTTP 200 and no profiling output appeared.
- A live Codex connection appeared during that clean-image observation and reached DTLS/SRTP/SCTP; the assistant did not start microphone capture. At 49.968 and 54.968 seconds, the task watchdog reported starved `IDLE0`, with `codex_mic` on CPU 0 and LVGL on CPU 1. The decoded capture backtraces were inside Opus encoding (`silk_noise_shape_analysis_FIX` / `opus_encode_native`, `speech_live.cpp:408`). This is an unresolved active-call scheduling/performance issue, not a passing sustained-duplex check or proof that OOM/latency reports are fully resolved; its cause was not established by this flash observation.

## Native rendering compiler/cache gains — 2026-10-12

- Changed tracked board/default and active local builds from ESP-IDF debug optimization (`-Og`) to standard size optimization (`-Os`), retaining assertions. Disassembly confirms the rotation loop now uses incremented pointers and a hardware loop instead of multiplying the destination index for every pixel. No animation-cadence, SPI-clock, codec-priority or buffer-size change.
- Aligned the existing PSRAM framebuffer to `CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE` (32 bytes on this board). Each 16-pixel adjacent source run fits one cache line rather than straddling two. No second retained framebuffer or larger internal DMA allocation.
- Same-process, pre-UI/voice rotation benchmark used identical pixels and 32 full-frame rotations per case: original debug/malloc 11,433 µs, size/malloc 10,823 µs, size/aligned 8,141 µs. Original allocation was eight bytes off a cache-line boundary; all three final-strip hashes matched (`3b09aea5`). Alignment alone reduced this isolated rotation cost by 24.8%; both changes reduced it by 28.8%.
- Matching idle board profiles measured mean rotation/transfer at approximately 26.9 → 23.3 ms and largest UI-handler duration at 46,155 → 41,158 µs. Startup internal free/largest heap increased from 114,663/40,960 to 132,499/57,344 bytes in profiling builds. The existing external-BSS memory repair remains enabled.
- Actual production-LVGL/art/fonts host smoke compared 2,400 snapshots across all twelve scenes, idle/listening/thinking/speaking/history/scenes/settings/live/muted/connecting, network/clock changes and sleep/wake: debug/size optimization and full/direct rendering hashes all matched. Every simulated physical panel pixel matched the rotated logical framebuffer. Light/dark native framebuffer captures were inspected; the physical LCD was not visually inspected.
- No permanent host timing assertion was added: host execution cannot regress the ESP32 cache-line or compiler/scheduler timing. The connected-board differential benchmark/profile is the performance evidence; existing public UI/rotation contracts are unchanged.
- A live Codex session appeared during the first size-only board observation and completed a native `get_device_status` call. UI-handler stalls reached 453,735 µs under that concurrent load; those samples are not an idle comparison or proof of improved active-call latency. The assistant did not initiate microphone capture. [INFERENCE] CPU/network/codec contention is a remaining candidate, not a proven or resolved cause; no claim of smooth sustained duplex rendering or long-session memory stability.
- Removed the temporary profilers, copied debug rotation, startup benchmark allocations and host/serial smoke files. Clean application image `0x448a20` bytes (10% slot headroom) built and application-only flashed at `0x10000` with write-hash verification, preserving NVS/Wi-Fi/accounts/partitions. A 40-second actual ESP-IDF monitor observation recorded LCD/codec/native-agent/web startup, internal free/largest heap of 132,547/59,392 bytes and no panic/allocation-failure/profiling output; monitor exited cleanly and Home returned HTTP 200.

## Codex delegation internal-memory repair — 2026-10-12

- Captured the actual failure after native live capture/Opus startup: `buddy_agent` could not allocate a 12,288-byte internal-RAM pthread stack (`caps=0x804`). Internal free/largest blocks were 5,327/5,120 bytes while PSRAM still had 6,282,868 bytes free. The decoded backtrace reaches Rust `CallbackModel::complete`, not the display framebuffer or microphone stack.
- Enabled external BSS in the tracked board/default and active local configurations. A speech-component linker fragment moves CPU-only `synthdata`, `synthesize` and `wavegen` BSS into PSRAM; ESP-IDF also applies its standard Wi-Fi/lwIP external-BSS mappings. Linked-symbol comparison shows 52,863 bytes moved out of internal RAM, including 38,785 bytes of local speech workspaces. DMA/task control blocks and network/model stacks remain internal; codec stacks, queue bounds and TLS/SDP-fingerprint checks are unchanged.
- On-board local eSpeak playback completed after relocation. Host production-engine smoke passed stereo/chunk constraints, playback-error propagation and recovery (`84123` frames; waveform hash `873f95beff37c051`); existing live protocol checks passed. Linked echo/frame/phoneme/harmonic/command buffers have PSRAM addresses.
- A temporary no-microphone device smoke held real authenticated Codex HTTPS, DTLS/SRTP/SCTP, sideband TLS, Opus encoder/decoder and their task/queue allocations while the real native agent executed `get_device_status`. Both model requests returned HTTP 200, the tool completed and the agent succeeded. Lowest sampled internal free heap was 18,775 bytes, with a largest block of 17,408 bytes; it recovered to 75,811 bytes after the agent completed. Startup internal free heap was 114,279 bytes in that profiling build. No allocation failure was logged in this scenario.
- Verification limit: the no-microphone read-aloud probe later received a provider SCTP abort, as it did before the memory change. This proves concurrent callback allocation/model/tool execution, not end-to-end Codex read-aloud, a post-fix physical microphone conversation, acoustic quality or long-session stability. No host test can exercise this firmware linker/physical-heap placement; the board smoke is the regression evidence.
- Removed all temporary allocation hooks, phase/callback logging, automatic diagnostic requests and smoke artifacts. Clean application image `0x4732f0` bytes (6% slot headroom) built and application-only flashed at `0x10000` with write-hash verification; partitions, Wi-Fi, accounts and saved NVS were not rewritten. A 40-second real ESP-IDF monitor observation recorded successful LCD/codec/agent/Wi-Fi/web startup, internal free/largest heap of 114,707/40,960 bytes, no automatic diagnostic call and orderly monitor exit. Home returned HTTP 200. No additional microphone session was started by the assistant.

## Native UI responsiveness — 2026-10-12

- Switched LVGL from forced full-screen redraws to direct-mode dirty-region rendering in the existing PSRAM framebuffer. The panel still receives one complete rotated frame, only after the last dirty region: its QSPI driver streams RAMWR/RAMWRC rather than addressing arbitrary partial-height rectangles. No second framebuffer or larger DMA allocation.
- Initialized the LCD ISR wake flag and explicitly yielded after DMA completion. ESP-IDF 5.5.3's SPI panel callback ignores the user's boolean return, so returning a wake flag alone does not resume LVGL before the next scheduler tick.
- Touch polling now uses the existing I²C1 touch helper, not the I²C0 audio/RTC bus wait. Unchanged voice/status snapshots no longer repeat mode/style updates; fitted labels reuse the existing unchanged-text guard.
- Connected-board serial profiling, excluding startup, measured the largest UI-handler duration at 102,098 µs originally, 59,097 µs with dirty rendering alone, and 42,145 µs with the integrated changes. Mean full-frame rotation/transfer fell from approximately 40.1 ms to 24.0 ms. Touch reads remained approximately 1.5 ms. These are idle animation measurements, not active Codex-call timing or physical tap latency.
- Native production-LVGL framebuffer smoke compared 620 frames across incremental captions, connecting/listening/speaking/mute/end, history, settings/scenes, sleep/wake and all twelve scenes: full-refresh and direct-mode pixel hashes matched. The original and optimized setters also matched all 260 transition frames. One hundred identical live snapshots fell from 400 allocations/seven dirty regions to zero allocations/zero dirty regions. Light/dark native framebuffer captures were inspected; the physical LCD was not visually inspected.
- Profiling firmware built and application-only USB writes passed hash verification after byte-for-byte partition-table comparison. Wi-Fi, accounts, NVS, filesystem and partitions were not rewritten. Codec/audio task priorities and microphone admission are unchanged; no live microphone session was started for these checks.
- Removed all temporary profiling and smoke artifacts. Clean firmware (`0x4731f0` bytes, 6% application-slot headroom) built and application-only flashed with write-hash verification. A 20-second actual ESP-IDF monitor observation produced no new serial output and exited cleanly; the deployed Home endpoint returned HTTP 200. This does not establish active-call audio performance or physical touchscreen feel.

## Direct Codex setup and connection repairs — 2026-10-11

- Replaced the LAN bridge with experimental on-device Codex device-code login and verified HTTPS/WebRTC/Opus voice. Owner-authenticated Settings on home Wi-Fi provides phone sign-in, cancellation, account replacement and sign-out; OAuth credentials remain in unencrypted NVS. No host bridge, paid STT fallback, HTTP native microphone control or native-history exposure.
- Sized voice signaling headers for bounded OAuth tokens/account IDs instead of a fixed 1 KiB TX buffer, and allowed the existing 30-second signaling window. Preserved incomplete sideband frames across nonfatal TLS read timeouts and separated delegation scratch from the frame-reassembly buffer.
- Raised the web server's bounded incoming-header limit from the private/default build's 512 bytes to 4096, allowing modern browser headers plus owner cookies without changing authentication or Origin checks. Home now distinguishes AP-only chat denial from an unreachable device, clears expired AP history and decodes the isolated silent browser snapshot.
- Corrected the reported `codex_mic` stack overflow: audio tasks use the pinned codec's recommended 40 KiB encoder / 20 KiB decoder stack budgets in PSRAM, rather than 4 KiB internal stacks. Existing network/peer stacks remain internal; Wi-Fi and saved accounts are not erased.
- Verification: integrated ESP-IDF firmware builds (`0x473140` bytes; 6% application-slot headroom); account scenarios pass under ASan/UBSan, and all six actual owner-auth scenarios pass. Real SDK header serialization/parser smoke covers large bearer tokens and browser-header acceptance/oversize rejection. Production-method sideband smoke covers delayed TLS payloads, interleaved delegation and retained frame deadlines. Actual Chromium verifies Home reachability/AP expiry and phone sign-in/manual-copy/cancellation/session revocation using explicit HTTP fixtures.
- Hardware limit: the user's logs reach SCTP and capture startup, then report the old mic-stack overflow. This build has not been flashed or microphone-tested by the assistant because the serial monitor holds the USB port; no passing physical audio or crash-free direct-call claim.

## Continuous Codex voice and duplex flow control — 2026-10-11

- Added persistent authenticated `/v1/arvel/live` between the ESP32 and Codex LAN bridge, following omp's OAuth/WebRTC/Opus transport, transcripts, echo/barge-in heuristic and device-action delegation. The existing Rust backend executes real device tools; ordinary voice conversation does not need a text-provider or STT credential.
- Native/web mic: first tap starts; subsequent taps mute/unmute upload without canceling replies or reconnecting. Native hold/browser End closes the call. Real incremental captions replace recording/preview/send in Codex mode; final history deduplicates mirrored events but preserves repeated later turns. Local recording and one-shot read-aloud remain available. Boot, reset, changed speech settings and failure do not auto-open the mic.
- Fixed the reported “live audio or context consumer is too slow” failure during normal replies: bounded PCM queues wait for consumer progress rather than terminating on their first full slot, with a two-second genuine-stall deadline. Input drains in bounded batches during playback; TCP_NODELAY, combined frame-header/payload writes and correct partial-write timeout handling avoid starving microphone upload.
- Passed 50 bridge tests and native WebSocket protocol checks. Application-only firmware (`0x416b50` bytes, 14% slot headroom) flashed with write-hash verification, preserving NVS, partitions, existing preferences and concurrent display changes. An actual device/codec smoke survived an immediate 256000-byte/four-second audio burst; mute stopped upload, unmute resumed the same call, and Stop cleaned up.
- Real Codex smoke completed two spoken turns on one session, returned 861440 PCM bytes, continued output while muted, ignored muted input and released ownership after Stop. Real device conversation completed a delegated `get_device_status`. Browser controls showed connected muted state, correct `aria-pressed`, unmute and End with no JavaScript errors. Physical touch/hold and subjective acoustic quality were not checked.

## Opposite landscape orientation — 2026-10-11

- Switched to the other 90-degree landscape orientation while keeping the logical screen at 640×172. Physical panel pixel `(x,y)` now reads logical pixel `(y,171-x)`; touch coordinates use the matching inverse mapping, including existing out-of-range clamping.
- Kept the 16-row DMA buffer, single SPI transaction slot and adjacent PSRAM reads. Native checks verified every pixel and matching touch position for 16-, 17-, 32- and 64-row strips, plus guard words, corners and out-of-range touch inputs.
- Built and flashed the application-only image (`0x4169c0` bytes) over USB with hash verification. Observed successful LCD, codec, Wi-Fi and web-server startup; free internal heap after initialization was 81,647 bytes. Physical screen appearance and manual taps were not visually inspected.

## Display RAM and rotation optimization — 2026-10-11

- Reduced the internal LCD DMA strip from 64 rows (22,016 bytes) to 16 rows (5,504 bytes), saving 16,512 bytes without another framebuffer. Reduced the SPI transaction queue from ten slots to one: the flush callback already waits for completion before reusing its single strip.
- Changed rotation to read adjacent pixels from PSRAM and scatter into internal DMA memory, preserving the existing pixel mapping and full-frame panel stream. On the USB-connected ESP32-S3, a temporary 120-frame profiler measured mean/max flush times of 74,640/77,167 microseconds originally, 80,967/85,428 with only the smaller buffer, and 40,420/43,700 after changing read order. The small-buffer-only slowdown is not retained.
- The board confirmed 8 MiB PSRAM at 80 MHz. Free internal heap after application initialization was 64,463 bytes initially and 81,583 bytes in the optimized profiling build; largest blocks were 30,720 and 31,744 bytes. Concurrent live-voice edits were included in later builds, so whole-application heap differences are not an isolated display-only comparison. A one-time startup log now reports internal free/largest blocks separately from free PSRAM.
- Native pixel checks covered all 110,080 output pixels with 16-, 32-, 64- and partial-final 17-row strips, including destination guard words. ESP-IDF builds and application-only USB writes passed hash verification; partition table, NVS and filesystem were not rewritten. Temporary frame profiling was removed. Physical LCD appearance and perceived voice-latency improvement are not established by these checks.
- Final uninstrumented firmware (`0x4169b0` bytes, 14% application-slot headroom) was flashed over USB with hash verification. A real chat requested “Memory check complete.” with spoken output enabled: it transitioned through thinking/speaking to idle in 18.07 seconds, returned the exact reply and reported no agent error. Combined free heap afterward was 7,564,056 bytes. This verifies the spoken-response path completes; it does not measure first-audio latency or subjective audio quality.

## Chat callback allocation crash — 2026-10-11

- Replaced Tokio's panic-on-failed `spawn_blocking` with fallible native callback threads. Reduced the ESP-IDF internal callback stack from 24 KiB to 12 KiB; each completed model/tool worker is joined before allocating the next, and timed-out workers are drained before RPC releases caller-owned context.
- Callback thread allocation failure is now an agent error rather than a firmware abort. Chat shows the actual agent error when there is no more specific transport error and clears stale transport errors before each run.
- Native production-RPC smoke injected `pthread_create` ENOMEM: model allocation failure returned JSON-RPC error, a subsequent run recovered, and tool allocation failure became canonical tool-error history followed by a completed model turn. The permanent cancellation test verifies that dropping the worker waits for a timed-out callback before releasing context.
- ESP-IDF build and application-only USB flash passed with write-hash verification (`0x411d00` bytes, 14% application-slot headroom); NVS and the partition table were retained. The board completed verified TLS for two chat submissions, including browser Ctrl+Enter, and reported provider HTTP 429 without a task-allocation panic or reboot. Successful model replies and on-board tool execution remain unverified for this build because the provider was rate limited.

## Battery reading — 2026-10-11

- `adc_bsp` now reads the battery: BAT_ADC on GPIO4 (ADC1 channel 3, 1:3 divider), 16-sample average, eFuse calibration when available, mutex-guarded. `battery_read` returns cell millivolts, a Li-ion resting-voltage percentage estimate and whether a cell is present; init no longer aborts on failure. Removed the unused vendor ADC demo.
- New `get_battery` agent tool (eighth of the runtime's eight tool slots); `get_device_status` includes the same battery object. The board has no charge-status signal, so results say the percentage is estimated and overstated while charging.
- Host test with a fake ADC covers the divider, averaging, curve interpolation, no-battery detection and the tool result through the Rust agent loop. Firmware builds with 14% application-slot headroom. Readings on real hardware are not verified.

## Device-management agent tools — 2026-10-11

- Registered seven agent tools (`device_tools.cpp`): `get_device_status`, `set_display` (brightness, night dimming, screen sleep, scene), `set_sound` (volume, voice, speaking speed, read aloud, captions), `set_profile` (personality, user name, time zone), `scan_wifi`, `connect_wifi` and `restart_device`.
- Setting changes reuse the settings panel's validation, persistence and live apply; rejections return the validation message to the model. Wi-Fi switches and restarts run only after the reply finishes, so they cannot cut off the reply. Wi-Fi passwords are never shown on screen.
- Moved Wi-Fi credential saving into `espwifi_store_station`/`espwifi_reconnect`, shared by the web app and the agent. The system prompt now describes the device tools.
- Host test through the real Rust agent loop: all seven definitions accepted, scripted calls produce the expected settings patches, validation errors and unknown arguments are reported, Wi-Fi switching is deferred and runs once. Firmware builds with 14% application-slot headroom. Not yet exercised against the live model or on hardware.

## Arvel design update: tool calls — 2026-10-11

- Thinking now lists the reply's latest two tool calls (design icon, verb, monospace target, spinner/check) in place of the quoted request; the chat log shows each reply's tool chips between question and answer, with the answer at the design's 13px.
- `unravel::Agent::set_tool_observer` reports tool start/finish; the chat worker maps tool names and arguments to the design's eight icon kinds and keeps the calls with each reply. No device tools are registered yet, so these surfaces stay hidden until one is.
- The asset generator now extracts the ToolIcon artwork (22/20px), a check icon, Inter 11/12/17 at weight 650 and Noto Sans Mono 11/12; the unused 19/600 face was dropped. Scene, character and icon pixels are unchanged.
- Firmware builds with 15% application-slot headroom; light/dark layouts were checked in a host LVGL render. Physical display rendering is not verified.

## Display and cold-clock reliability

- Kept sleep and night dimming independent; documented that Never sleep alone does not prevent the automatic 22:00–07:00 brightness reduction. Disabled night dimming on the connected device while retaining its selected brightness, sleep policy and time zone.
- Configured three NTP providers and extended the cold-clock gate to cover alternate-server handoff. Reconnects and new invalid-clock requests restart pending SNTP instead of inheriting stale backoff; valid clocks are still reused, with no fabricated time or TLS verification bypass.
- Controlled native smoke reproduced premature clock failure before the change and passed delayed completion, restart/reconnect recovery, offline/provider failure and deadline boundaries afterward. Real host UDP queries reached all three NTP servers; connected settings/browser controls and the ESP-IDF firmware build were verified. The updated clock firmware was built, not flashed; physical illumination and on-board cold-start recovery are not claimed.

## Unravel Buddy desk companion — 2026-10-08

- Replaced the native factory demo with Buddy home, state-driven face, UTC/network status, conversation, portrait keyboard and settings; removed obsolete generated screens and RGB assets.
- Added real typed chat and one shared worker-owned Rust conversation for native/web clients, latest-eight display history, asynchronous reset and nonblocking HTTP submission.
- Added explicit Talk/Stop/Send voice/Discard controls. Typed input does not require STT; optional browser read-aloud uses local eSpeak and checked codec writes.
- Fixed the one-shot SNTP completion check by using actual system time; retained verified TLS.
- Removed the unused startup factory scan task after a measured stack-overflow reboot.
- Moved UI/TLS heap buffers to PSRAM, selected a measured 24 KiB internal HTTPS stack, and reserved outbound network capacity with a 16-socket pool.
- Fixed quadratic LVGL long-word wrapping without truncating text or weakening the watchdog; added deterministic glyph-work and UTF-8/word-boundary regression tests.
- Verified actual V1 GLM conversation retention, reset, input/origin/busy rejection and a full 8192-byte prompt with the browser connected, plus browser chat/read-aloud, mobile reflow and native-source rendering/input invariants. HTTPS stack retained >20 KiB unused; firmware has 33% application-slot headroom.
- Physical display image/touch/microphone, audible speaker output, authenticated microphone transcription and battery-only startup remain unverified. Full frontend type-check retains inherited template errors; production assets build successfully.

## Ollama Cloud GLM 5.3

- Changed the firmware fallback, menuconfig default and configured model to `glm-5.3`, the direct Cloud API identifier for the local CLI's `glm-5.3:cloud`.
- Verified an authenticated Cloud request returned HTTP 200 and `GLM 5.3 is ready.`; rebuilt the private V1 ESP-IDF firmware successfully with 12% application-partition headroom.
- Fixed the agent interface starting hidden over the factory carousel; it now opens on boot with a consistent **Hide Chat** toggle label.
- Verified on V1 hardware: `Model set to: glm-5.3`, visible chat-panel state, a complete initialized-UI LCD transfer, Wi-Fi/codec/web-server startup and web HTTP 200. Removed temporary display probes; physical display appearance still needs user confirmation.

## On-device Rust runtime and board revisions — 2026-10-08

- Integrated the real Unravel Rust agent loop as an ESP32-S3 static library, using a reusable C++ `unravel::Agent` binding and in-process JSON-RPC.
- Added C++ tool registration/execution and canonical session retention; migrated the chat worker from direct completion orchestration to Rust.
- Added V1/V2 menuconfig selection. V2 reset/backlight-enable use the TCA9554; PWM moves to GPIO42.
- Moved checked battery power-latch initialization before display/network startup; retained speaker-amplifier enable.
- Removed the plaintext API credential; configure API key/model in menuconfig and rotate the exposed old key.
- Enlarged factory and OTA application slots to 4 MiB. USB partition-table migration is required.
- Host smoke exercised C++ tool execution through Rust, continuation, error-session retention, truncation rejection, reset and RPC IDs. Physical hardware/live-model verification is not claimed.


## Overview
Streamlined `user_app` component to focus exclusively on AI Audio Chat functionality, removing all test and demo code.

## Changes Made

### Removed Components
1. **Test Tasks** - All removed:
   - `example_color_task` - RGB color cycling demo
   - `example_button_boot_task` - Button event handling for demos
   - `example_button_pwr_task` - Power button management
   - `example_user_task` - RTC, IMU, ADC monitoring
   - `example_sdcard_task` - SD card testing
   - `example_scan_wifi_ble_task` - WiFi/BLE scanning demo
   - `example_test_touch_task` - Touch canvas demo
   - `i2s_audio_Test` - Audio recording/playback test
   - `power_Test` - Battery power detection

2. **Callback Functions** - Removed:
   - `audio_Test_Callback` - Audio test buttons
   - `lvgl_obj_event_callback` - Slider/brightness control
   - `set_canvas_color` - Touch canvas drawing
   - `set_canvas_fill` - Canvas clearing

3. **Dependencies Removed**:
   - `sdcard_bsp.h` - SD card operations
   - `button_bsp.h` - Physical button handling
   - `adc_bsp.h` - Analog-to-digital converter
   - `i2c_equipment.h` - RTC, IMU sensors
   - `ble_scan_bsp.h` - Bluetooth scanning
   - `esp_timer.h`, `esp_http_client.h`, `esp_crt_bundle.h`, `esp_sntp.h`
   - `cJSON.h`, `errno.h`
   - Standard library: `<string>`, `<cstring>`, `<cstdlib>`, `<time.h>`, `<sys/time.h>`

4. **Global Variables Removed**:
   - `app_touch_data_queue` - Touch event queue
   - `app_touch_group` - Touch event group
   - `audio_ptr` - Large audio buffer (192KB)
   - `lvgl_button_groups` - Button event synchronization
   - `is_vbatpowerflag` - Battery power state

5. **Header File**:
   - Removed `app_touch_t` structure definition
   - Removed `app_touch_data_queue` extern declaration
   - Removed `freertos/FreeRTOS.h` include

### Kept Components
1. **Essential Initialization**:
   - UI setup (`setup_ui`)
   - WiFi initialization (`espwifi_init`)
   - AI Chat component with OpenRouter API
   - LCD backlight PWM
   - I/O expander for power management
   - Audio codec initialization

2. **Key Settings**:
   - **API Key**: Configured for OpenRouter
   - **Model**: `google/gemini-2.0-flash-exp` (supports native audio input)
   - **Audio**: ES8311 DAC + ES7210 ADC at 16kHz/2ch/16-bit

## File Sizes
- **Before**: 453 lines (user_app.cpp)
- **After**: 60 lines (user_app.cpp)
- **Reduction**: 86% smaller

## Benefits
1. ✅ **Cleaner code** - No test/demo clutter
2. ✅ **Lower memory usage** - No 192KB audio buffer allocation
3. ✅ **Fewer tasks** - Only essential background tasks run
4. ✅ **Faster initialization** - No sensor/peripheral setup delays
5. ✅ **Single purpose** - Dedicated AI audio chat device
6. ✅ **Easier maintenance** - Minimal surface area for bugs

## Testing Notes
- The AI Chat UI is created by `ai_chat_bsp` component
- Voice button and output area are managed automatically
- All audio operations handled by background tasks in `ai_chat_bsp`
- No manual task creation needed in user_app

## Migration Notes
If you need any removed functionality:
- **Audio testing**: Check `components/audio_bsp/` examples
- **Sensor data**: See `components/i2c_equipment/`
- **SD card**: See `components/sdcard_bsp/`
- **Touch drawing**: See original git history for canvas code
- **Button handling**: See `components/button_bsp/`

## Next Steps
Consider further UI optimization by simplifying the LVGL screens in `setup_ui()` to show only:
- AI Chat conversation area
- Voice input button
- Status/connection indicator
- Optional: Battery level, WiFi indicator

## 2026-10-08 — Ollama Cloud and local voice replies

- Replace OpenRouter with Ollama Cloud's verified-TLS OpenAI-compatible endpoint; separate Ollama and transcription credentials in menuconfig.
- Preserve microphone input through streamed multipart WAV transcription, then pass only the validated transcript to the actual on-device Rust agent.
- Add local English eSpeak-NG speech, bounded streaming PCM and 22050-to-16000 Hz stereo conversion through checked codec writes.
- Transfer the original PSRAM recording into the request queue; free it before inference. Keep capture/preview ownership until playback finishes and fail checked capture on codec errors.
- Expand all app slots to 4.75 MiB while preserving the 1 MiB filesystem. USB partition migration and a device-file backup are required.
- Keep the saved Ollama key in ignored private build configuration; never publish credential-bearing firmware or generated headers.
- Verified native waveform generation/determinism, playback-error propagation/recovery, STT/Rust/TTS fixture flow, live Rust/Ollama Cloud HTTP 200 and a complete V1 ESP-IDF build. Device/audio/battery runtime checks and authenticated STT await the connected-device credential setup.
