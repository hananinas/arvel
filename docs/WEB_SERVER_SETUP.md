# Firmware web server, Wi-Fi setup and owner login

## Authority

The firmware serves public embedded Svelte assets over HTTP, but **Settings, native device state and controls require a real owner username/password session**. A matching Origin, page load or displayed template feature alone grants no authority. This is a trusted-home-LAN device interface, not an internet-facing service or host console.

Native conversation, microphone, transcripts and effects remain separate from legacy HTTP chat. Owner login permits saved model/deployment/speech/search/Niko configuration, bounded history inspection and explicit native controls. Home draws actual projected state, not framebuffer pixels; no raw-touch endpoint exists. Starting voice asks consent for Arvel's physical microphone, never the browser mic. Native-audio typed conversation/read-aloud is explicitly unsupported. Wi-Fi onboarding/zero-tool legacy chat remain physical-window/AP-only; host console/runtime-pairing authority remains unimplemented.

## Connect an unconfigured board

1. Open native **Settings → Device → Wi-Fi setup**.
2. Read the fresh **eight-digit WPA2 password** on the physical screen. Enter every digit, including leading zeros, with no spaces. Four digits cannot be used as a WPA2 Wi-Fi password.
3. Join **Unravel-Agent** with that password and open `http://192.168.4.1/wifi/sta`.
4. Save the station network. An empty password for an unchanged SSID preserves its saved password.
5. Rejoin that home Wi-Fi on your phone/computer. Create the owner web login below, then configure API keys; there is no need to rush those settings before the AP closes.

The setup window lasts five minutes. **New password** replaces the code and disconnects old AP peers; reconnect with the new digits. **Stop setup**, expiry or a successful onboarding station connection closes the AP. Passwords disappear from the physical setup screen when the window closes; they are not returned by HTTP status APIs or logged. The screen shows numbered join/open steps, a large eight-digit password, remaining time and 44-pixel-high controls. **Retry** recovers from open failure.

The owner selected eight numeric digits for easier entry instead of the original long random key. A captured WPA2 handshake is more vulnerable to offline cracking; captured HTTP traffic, including any owner login or API keys submitted on that AP, could then be decrypted even after expiry. Use only a trusted nearby environment and Stop setup when finished. The network remains WPA2-protected, not an open hotspot with a four-digit webpage PIN.

Without a saved network the board remains unconfigured. Boot and station connectivity loss do not start a shared-password hotspot. Normal operation is station-only. Rotate any real station credential previously committed to source; removing the fallback does not rotate that credential.

Only sockets addressed to the AP interface during the physical window can use Wi-Fi onboarding and legacy chat APIs; station/LAN access to those APIs denies even while the AP is active. Settings uses the owner session instead, on AP or station/LAN, and is not closed by AP expiry. Possession of the Wi-Fi setup password does not grant owner Settings access.

The Wi-Fi/legacy-chat AP guard accepts ordinary IPv4 and IPv4-mapped IPv6 sockets from ESP-IDF's dual-stack HTTP listener. Earlier firmware incorrectly returned `physical_local_access_required` despite a valid AP connection; this correction preserves AP-only admission for those APIs. Use the home-network address for normal authenticated Settings. A reachable static page alone does not establish API access.

On home Wi-Fi, `GET /rest/agent` returning `403 physical_local_access_required` is expected: legacy browser chat is AP-only, not a device-offline signal. Owner-authenticated Home (`/`), Settings and Codex setup do not need an AP window. For localhost preview/control, run the [development proxy](../README.md#cloud-credentials-and-desk-buddy-chat), log in to the device owner account, then open `http://127.0.0.1:5173/`.

**HTTP 431 / request headers too long:** older firmware used a 512-byte HTTPD request-header limit, which can reject ordinary browser requests even after Wi-Fi joins and the web server starts. Current startup explicitly permits a bounded 4096-byte header buffer; use the updated firmware, not weakened authentication/Origin guards. This sizing correction is separate from the expected AP-only legacy-chat 403.

## Create the web login once

1. On Arvel, open **Settings → Device → Web login**.
2. On your phone/computer, visit the `http://<home-network-IP>/settings` address shown on that screen. Stay on the same home Wi-Fi.
3. Enter the **six-digit Web login approval code**, choose a username and password, and create the login. This is not the eight-digit Wi-Fi password.
4. Keep **Remember this browser** checked only on your own device. Configure your provider or connect Codex under **Standalone read-aloud & Codex account**, and bookmark this address.

The approval code lasts five minutes, accepts at most five incorrect attempts and is never returned by HTTP or logged. **New code**, **Cancel**, expiry or successful account creation/recovery closes/replaces approval and clears the physical display; normal browser login does not need physical approval. Approval never starts an AP or disconnects station Wi-Fi.

Usernames are 1–32 ASCII letters, digits, dots, underscores or hyphens; passwords are 8–128 UTF-8 bytes. There are no shared/default credentials. **Log out** revokes that browser's session and a previously authorized logout stops shared native activity; an unauthenticated logout cannot stop another session. Logout cannot undo a command already handed to a controller.

**Forgot password?** plus a new physical approval code replaces owner credentials and revokes all sessions. Committed Codex accounts, provider/Niko keys and Wi-Fi remain, but pending Codex sign-in and native activity are cancelled. Factory reset still requires its separate physical confirmation.

## Session and HTTP security

Password storage uses PBKDF2-HMAC-SHA256 with 100,000 iterations and a fresh 16-byte salt. Four bounded sessions use random 256-bit tokens; only their SHA-256 hashes are stored. Cookies are `HttpOnly`, `SameSite=Strict` and path `/`; tokens are not put into JSON, URLs or localStorage. Remembered sessions persist in NVS until logout, recovery or slot replacement, with a one-year browser-cookie maximum age but **no independent server calendar expiry**. Unremembered sessions last up to eight hours and end on device restart.

Five failed logins within a minute trigger a 60-second global cooldown. Successful login does not replenish that failure budget. Logout/recovery/persistent session issuance reports storage failures instead of pretending persistence succeeded.

**HTTP is unencrypted.** A LAN attacker able to intercept traffic can steal login passwords, session cookies, submitted API keys and the displayed OpenAI device sign-in code. Browser-cookie flags, password hashing, Origin checks and login throttling do not encrypt the connection. Use a trusted home network, a unique password, no port forwarding and no internet exposure. OpenAI sign-in and outbound provider/Codex authentication/signaling still use verified HTTPS; WebRTC audio is encrypted. This HTTP exception grants no host execution authority.

## HTTP contracts

| Route | Behavior |
| --- | --- |
| Static pages | Serve embedded assets; a displayed template feature is not proof of an implemented API |
| `GET /rest/device` | Owner-authenticated native snapshot: conversation/transcripts, scene/status/clock, voice, screen and RAM metrics; authorization rechecked before returning |
| `POST /rest/device/control` | Exactly one `action`: `voice_toggle`, `voice_stop`, `screen_toggle` or `brightness_cycle`; firmware assigns native authority |
| `POST /rest/device/chat` | Owner native text with optional `speak`; Home sends silently. Native-audio mode refuses rather than forwarding to the text model |
| `POST /rest/device/reset` | Starts a new native conversation identity; no archive deletion or old-action replay |
| `GET /rest/device/history` | Owner conversation summaries, bounded cursor pages |
| `GET /rest/device/history/<uuid>` | Owner final message/tool/source history; bounded literal/time-filtered pages, marked untrusted |
| `GET /rest/device/history/<uuid>/export?kind=…` | One bounded page of `messages`, `session_boundaries`, `tool_calls`, `sources` or `tool_evidence` |
| `POST /rest/device/history/new`, `/<uuid>/resume`, `/<uuid>/close` | Owner memory control with empty JSON object; resume restores bounded context, never microphone/old actions |
| `DELETE /rest/device/history/<uuid>` | Confirmed owner logical deletion, not secure SD erasure |
| `POST /rest/agent/readiness` | Owner saved native-deployment health check only; no microphone/inference request |
| `GET /rest/device/niko?after=…&count=…` | Owner bounded controller status/inventory, including stale/auth/expiry/observation state |
| `POST /rest/device/niko/control` | One exact discovered owner-enabled supported dimmer UUID; absolute `status` and/or `brightness`, shared durable tool worker |
| `GET /rest/agent` | Physical-window/AP-only legacy history and status, never native history/transcripts/settings |
| `POST /rest/agent/chat` | Physical-window/AP-only silent text in a separate zero-tool session; speech requests deny |
| `POST /rest/agent/reset` | Physical-window/AP-only reset of legacy history, not native chat |
| `GET/POST /rest/agent/settings` | Owner session required on AP or station/LAN; saved keys never returned |
| `/rest/agent/voice` | `403`; no microphone control or native transcript access |
| Wi-Fi settings/status/scan | Physical-window/AP-only onboarding access |
| `POST /rest/restart`, `POST /rest/factoryReset` | Always `403 physical_confirmation_required`; no side effects |
| `GET /rest/auth/status` | Non-sensitive configured/authenticated/approval/storage availability; username only for an authenticated session |
| `POST /rest/auth/setup` | Physical-code-approved first-account creation or recovery; replaces credentials and revokes old sessions |
| `POST /rest/auth/login` | Username/password login; issues an owner cookie, subject to the shared cooldown |
| `POST /rest/auth/logout` | Revokes the current session; persistent-storage failure is an error, not a successful logout |
| `GET /rest/codex/status` | Owner session required; account state, temporary sign-in code/expiry and sanitized error only, never OAuth tokens |
| `POST /rest/codex/connect`, `/cancel`, `/disconnect` | Owner session and matching Origin required; start/cancel sign-in or clear saved account, never start the mic |
| Other unimplemented template endpoints | `501 not_implemented` |

Auth/Settings/native writes require a present matching Origin and complete bounded JSON, never wildcard CORS. Auth bodies are capped at **1024 bytes**, Settings at **48 KiB**, and ordinary native/legacy JSON at **9 KiB** with a separate 8192-byte text limit. Settings can report capacity/storage failure; the existing 24 KiB NVS partition is not expanded, and multi-blob updates are not claimed crash-atomic. Recheck saved state after an unconfirmed save before retrying.

Owner authorization is rechecked after body reception and before private snapshots return. Protected queued work privately copies the canonical session token, rechecks authority/generation before execution and never exposes that token to models, tool arguments, archive, UI or logs. This admission/Stop behavior is not a promise that browser-cookie expiry immediately closes an already established live voice session. Unknown control actions, extra fields and client `source` values refuse. Legacy writes retain AP-window checks; framebuffer/touch routes remain absent.

History queries use `after` (nonnegative cursor), `count` (1–20), and for message pages `literal` (at most 256 UTF-8 bytes), `utcFrom`/`utcTo` (nonnegative safe integer milliseconds). Unknown/duplicate query keys, invalid encoding/numbers and oversized requests refuse; raw query is bounded to 2048 bytes and HTTP URI capacity is 2304. Data pages stay within **20 rows/48 KiB** and authorization/cancellation is checked while reading. Home streams all five export kinds into one browser-owned JSON Lines file; each line after its version/conversation header contains a kind and bounded page. Close first for a stable export. Absent authoritative UTC/request IDs are null/unavailable, not fabricated dates or IDs.

Native memory/Niko/health work is serialized with the existing owner worker; queue/operation pressure is a visible error. Asynchronous acceptance is **HTTP 202 with `accepted: true`**, not success or a physical-effect acknowledgment. Niko accepts only `device_id`, optional `status` (`On`/`Off`) and optional integer `brightness` (0–100), with at least one desired property. It never accepts broker/topic/credentials or arbitrary scene/multi-target/toggle operations. Current write classification requires official `action` / `nikohomecontrol` / `dimmer` metadata and authoritative writable property definitions. Generic relays/sockets and other classes are read-only regardless of name.

Restart and Factory reset are available under native **Settings → Device → Device actions**. Each requires a separate physical confirmation with Cancel. The screen reports request acceptance, not observed reboot completion. Stop or expiry cancels scheduled effects; failed NVS erase is reported without restarting.

## Provider and speech provisioning

Use authenticated browser **Settings** on home Wi-Fi to add or replace chat/STT keys and endpoints/models, select speech mode/voice, and save profile/preferences. Keys are write-only; reads return configured flags. Changing a chat/STT destination requires a replacement key or explicit clearing so a saved key cannot be forwarded to a different URL. Successful saves persist in NVS; busy/storage errors do not silently mutate live settings. A revoked/expired session hides the Settings forms and offers login, preserving non-sensitive drafts while clearing entered keys. Re-enter any unsaved key after login. No AP reopening is necessary.

For Codex, select **Connect Codex**, open [OpenAI device sign-in](https://auth.openai.com/codex/device) on your phone, enter the displayed one-time code and approve the subscription account. Enter the code only at **auth.openai.com**. Enable device-code login under ChatGPT **Settings → Security** if asked; workspace policy may require administrator approval. After **Codex account saved**, choose **Conversation transport: Codex subscription · direct live voice**, choose a voice and **Save**. The standalone read-aloud backend does not select this transport. Start/mute explicitly with the physical mic or owner-authenticated Home; hold the mic, use **Stop device voice**, or say “end” to finish. Opening the page never records audio, and there is no background cloud wake listener. Intentional stop reports a clean ended state; unexpected disconnects keep their specific reason and require explicit reconnection. No host, CLI, bridge URL/key or separate STT API key is needed for ordinary Codex conversation. Delegated device tools still use the configured text provider.

Direct voice is experimental: account/workspace access and subscription limits apply, and successful device-code login does not prove live voice eligibility. Production code uses fixed OpenAI auth/live endpoints. The account survives restart, page refresh and token refresh until explicit sign-out/account replacement or factory reset; choosing Local retains it. Cancelling replacement sign-in keeps the committed account. Sign-out removes saved credentials, cancels pending sign-in and ends current Codex voice. Verified outbound HTTPS checks certificate chain, hostname and lifetime, refuses redirects and requires a valid clock; WebRTC validates the peer against the authenticated SDP fingerprint. See [the direct voice setup](../README.md#experimental-direct-codex-subscription-voice).

The owner password is stored only as a salted derivation and owner session tokens as hashes. **Codex OAuth access/refresh credentials and provider/STT keys are recoverable from currently unencrypted flash/NVS** by an attacker with physical access. Keep the board physically trusted. Ignored private firmware defaults remain embedded credentials; `CONFIG_UNRAVEL_TTS_CODEX` and `CONFIG_UNRAVEL_TTS_VOICE` only select boot speech mode/voice, not an account login. Never distribute credential-bearing private configuration, firmware or generated headers. Runtime device identity must not become owner authority.

## Native deployment, SD and Niko setup

1. Follow [VOICECHAT_DEPLOYMENT.md](../scripts/VOICECHAT_DEPLOYMENT.md) for owner-authorized Nebius GPU access, quota/spend limits, exact immutable artifacts, private verified TLS/bearer ingress and separate NVIDIA evaluation/public-use permissions. No hosted endpoint or successful GPU run is inferred from the catalog.
2. Choose **Conversation model & deployment** explicitly. The models.dev picker shows capabilities, known adapter support and unknown metadata without inventing support/free pricing. Native speech requires its saved exact WSS profile; it has no separate STT/TTS configuration. **Check saved deployment · no microphone** establishes authenticated health only.
3. Insert SD before startup. **Local memory** shows actual storage status; Home can inspect/search/page/new/resume/close/export/delete. Missing/full/corrupt/read-only storage never automatically formats. Existing local tools keep bounded non-durable protection; Niko writes require durable admission. Selected context/audio goes to cloud; SD/NVS/LAN HTTP are not end-to-end encrypted.
4. Enable Niko Hobby API in programming software. Privately enter the supplied username, annual token, CA and controller/TLS identity. Arvel uses verified MQTT TLS **8884**; valid time and certificate identity remain required. See [public Hobby API setup](https://guide.niko.eu/en/connhc2/lv/hobby-api); do not redistribute confidential documentation or the supplied CA.
5. Save enabled Niko configuration for read-only discovery, refresh inventory, then explicitly enable allowed supported dimmers and save permissions. The default allowlist is empty. Owner control asks confirmation; acceptance is not success. Inspect Home's actual tool outcome and controller observation; only `succeeded` is marked successful. Unknown/partial/failed/cancelled remain distinct.
6. Controller/TLS-identity changes clear entered credentials and draft permissions, requiring all fresh credentials or disabling/explicit clearing. Keys are wiped from browser drafts after submission, failure, logout and unmount. Firmware returns configured flags, never saved credentials.

Niko subscribes/discovers before writing and publishes one non-retained absolute command. Evidence must arrive after publication returns and on the same connection epoch; a queued earlier match or disconnect is unknown, not green success. Reconnect is read-only and never replays a control. An observation is not proof of causality or a physically lit lamp. Account/runtime cancellation cannot recall handed-off MQTT/TLS bytes.

Before installing this candidate, follow the [approved two-slot partition migration](../README.md#build-and-board-selection). It preserves data addresses but requires bootloader/table/initial OTA metadata/application together, with backups and explicit flash authorization. App-only updates are insufficient for the old table. No flash/erase or household action was performed during this implementation.


## Build and integration

The web server starts after Wi-Fi initialization in `components/user_app/`. Its implementation is `components/web_server/web_server.cpp`. Public control remains `web_server_start()` / `web_server_stop()` in `web_server.h`.

Embedded assets are generated from `interface/` into `main/WWWData.h`; never hand-edit that header. See [INTEGRATION_NOTES.md](INTEGRATION_NOTES.md) for build layout and the root README for ESP-IDF/toolchain/partition instructions. Frontend input invalidation and fully locked reproducible release builds remain limitations.

## Verification limits
**Current candidate, 2026-10-14:** actual Chromium with isolated responses exercised all six tool states, reachable Stop during busy voice, real host-store data pagination/45-message five-kind streamed export, saved-profile health without capture, relay write refusal, cancelled/confirmed dimmer requests and controller-hop secret/allowlist clearing. At 320 pixels, document width stayed 320 with no duplicate IDs or page errors. Fixtures never reached the physical board/controller or a paid model. Current firmware compiles with 16% headroom in 6 MiB slots but is unflashed; host `web_auth` checks below do not prove the new firmware HTTP routes, queued native start/revocation, actual SD/NVS faults or acoustic/household behavior. Public distribution/demo rights remain unresolved in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).


Six permanent host scenarios exercise the actual `web_auth` core with OpenSSL and explicit IDF/NVS/clock fixtures: physical-first creation, restart/session semantics, throttling, recovery/revocation, approval boundaries and failed/corrupt/unavailable storage. Run `CC=gcc bash components/web_auth/test/run.sh`; OpenSSL development headers/library and pthreads are required. The fixture leaves disk unchanged on a simulated failed commit; real NVS writes may become visible after reboot despite a reported error, so this is not hardware power-loss/fault proof.

The real Codex authentication core has a separate host suite: `bash components/codex_auth/test/run.sh` (existing C/C++ compilers, pthreads and downloaded cJSON required). It models HTTP/NVS/clock behavior, including cancellation, refresh and storage failures; it does not prove actual OpenAI eligibility, TLS/DNS, ESP32 flash power-loss behavior or physical microphone/speaker operation. Earlier voice/owner-login evidence below is historical, not a passing claim for the new direct voice transport.

Real production HTTP auth/Settings handlers passed absent/foreign-Origin denial, cookie parsing, unauthenticated admission denial, expiry during body reception, remembered backend restart, throttling, recovery and honest failed logout. These use an HTTP/NVS harness and redacted Settings save spies, not the board's provider persistence path. Actual Chromium verified first creation/logout/relogin, API/Codex form saves/key clearing, remembered restart, recovery, revoked-session draft retention, failed logout, throttle UI and 320-pixel mobile reflow without JavaScript errors. A logout-after-creation regression failed before the fix and passed afterward. Native LVGL pointer/framebuffer smokes verified approval lifecycle, secret clearing, station URL, offline/error copy and light/dark layouts; images were inspected.

Application-only USB flashing of the integrated owner-login firmware verified the written hash after byte-for-byte partition-table comparison, without overwriting NVS/filesystem regions. A 20-second connected-board serial observation reached port-80 web-server, native-agent and application-ready without panic/watchdog evidence. These checks do not establish manual WPA2 join, on-board account creation/recovery timing, browser login on the physical board, physical touch, audible speech, certificate rejection or observed factory reset.

The P0 hardware acceptance gate remains required on an isolated designated board before host authority is enabled. Do not infer that a successful firmware build or template page load completes that gate.
