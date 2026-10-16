# Voice input and optional spoken replies

## Pipeline

Select one conversation transport explicitly. **Text pipeline** records/reviews audio, uses the saved STT service and text-provider Rust loop, then optional local/selected read-aloud. **Codex subscription voice** uses its saved OAuth account, `gpt-live-1-codex`, Opus and verified encrypted WebRTC; ordinary conversation is direct and free-text device delegations use the text agent. **Native audio** uses the released NVIDIA Labs VoiceChat 11B deployment's own speech/reasoning and structured calls through the same Rust/C++ registry, with no second model, STT or TTS service. No transport silently falls back to another.

## NVIDIA Labs 11B native audio

Provision only an authorized, quota/spend-capped Nebius GPU deployment using [the deployment instructions](../../scripts/VOICECHAT_DEPLOYMENT.md). Published model/image pins are not yet GPU-tested here. This is the released **NVIDIA-NemotronLabs-VoiceChat-11B**, not the different early-access 12B demo or Nebius Token Factory text API. The model license does not authorize the separate evaluation container or public demonstration; see [redistribution prerequisites](../../THIRD_PARTY_NOTICES.md).

In owner Settings select **NVIDIA Labs 11B · native speech**, save the explicit hosting-provider identity, exact verified `wss://host[:port]/v1/realtime` and write-only bearer key. Only the implemented NemotronLabs PCM realtime protocol/model contract is accepted. Endpoint/provider changes require a new key or explicit clearing; DNS/IPv4 ingress is supported, IPv6/redirects/query/fragment/URL credentials are not. Check the **saved deployment · no microphone** first: authenticated `/health` must identify the real Triton service and readiness. Health/configured status is not GPU inference, audible speech or a tool-execution proof.

Tap the physical mic or explicitly consent through owner Home to start capture. Tap again to mute/unmute; End/native hold stops the shared activity. Mute discards queued input and gates later frames while output can continue; bytes already handed to TLS cannot be recalled. Boot, page load, health, resume and reconnect never automatically start capture. Typed conversation and arbitrary standalone read-aloud are unsupported in this mode; switch explicitly to the saved text/read-aloud profile.

`speech_native.cpp` owns verified TLS/WebSocket control, capture and playback separately. The model contract is **24 kHz mono PCM16**; the physical codec stays **16 kHz stereo PCM16**. Fixed-state rational FIR conversion and 80 ms frames avoid an archive/full-response audio allocation. Fragmented messages, control events, audio and transcript storage are bounded; pressure/oversize/format failures stay visible. Callbacks copy into a bounded generation-fenced worker queue and never run tools, SQLite or LVGL in an audio callback.

Partial captions update in place. Only final text and actual tool/source outcomes enter available SD memory; interruption labels do not assert every word played. Complete calls retain actual provider call IDs and return exact SDK outcomes through the same 16-tool registry. Durable admission/terminal acknowledgment precedes durable success; Stop/revocation is checked before effects, but cannot undo a handed-off command. Unknown actions are not replayed on resume/reconnect. Native sessions do not claim a measured unlimited duration, server rollover/resume support, acoustic echo cancellation or a passing 60-minute soak.

The host native protocol/media smoke exercises production parsing/conversion, fragmented frames, repeated transcript deltas, cancellation and capacity boundaries. Actual physical microphone/speaker operation, barge-in quality, authenticated NVIDIA inference/tools and sustained duplex remain unverified; synthetic/host PCM is not acoustic proof.


## Configuration

Activate ESP-IDF, then run:

```sh
idf.py -D SDKCONFIG=sdkconfig.local -D SDKCONFIG_DEFAULTS=sdkconfig.defaults menuconfig
```

In **Unravel Agent**, set:

- **Ollama Cloud API key** (`CONFIG_UNRAVEL_OLLAMA_API_KEY`): text-pipeline/Codex-delegation profile only; obtain privately from https://ollama.com/settings/keys.
- **Ollama Cloud model identifier** (`CONFIG_UNRAVEL_OLLAMA_MODEL`): default `glm-5.3`, the direct Cloud API name for the CLI's `glm-5.3:cloud`.
- **Speech-to-text HTTPS transcription endpoint** (`CONFIG_UNRAVEL_STT_URL`): local voice mode only; default `https://api.openai.com/v1/audio/transcriptions`.
- **Speech-to-text API key** (`CONFIG_UNRAVEL_STT_API_KEY`): text-pipeline voice only, separate from the text-provider key; Codex/native audio do not use it.
- **Speech-to-text model identifier** (`CONFIG_UNRAVEL_STT_MODEL`): local voice mode only; default `gpt-4o-mini-transcribe`.

A compatible self-hosted HTTPS transcription service may replace OpenAI. It must accept multipart `model`, `response_format=json`, and `file` fields and return JSON with a string `text` field. Its certificate must be trusted by the firmware's certificate bundle. Redirects are refused to avoid forwarding credentials.

Keys saved through owner Settings reside in NVS; optional boot defaults remain embedded in firmware/generated headers. `.env`, `sdkconfig.local`, its backup and build outputs are ignored; never distribute credential-bearing artifacts. Text-pipeline voice needs text/STT credentials. Codex ordinary conversation needs its subscription account; its free-text delegations use the separately configured text provider, which may charge. Native audio needs only its authorized deployment bearer key for speech/reasoning; enabled search has independent credentials/costs.

Follow [direct Codex subscription setup](../../README.md#experimental-direct-codex-subscription-voice): after home Wi-Fi connects, open owner-authenticated **Settings → Standalone read-aloud & Codex account**, select **Connect Codex**, open `https://auth.openai.com/codex/device` on your phone, enter the displayed one-time code there and approve. Enable ChatGPT **Settings → Security** device-code login if asked; workspace policy may require administrator approval. Once the account is saved, select **Conversation transport: Codex subscription · direct live voice**, choose a voice and **Save**. Standalone read-aloud is independent. No bridge URL/key or STT API key is entered. First owner creation/recovery requires physical **Settings → Device → Web login** approval; remembered browser use does not require an AP.

The account persists across restart/page refresh; token refresh preserves it, and selecting Local does not sign out. Cancelled replacement sign-in keeps the committed account; owner recovery preserves that account but cancels pending sign-in. Explicit **Sign out of Codex** clears credentials and ends current Codex voice. OAuth access/refresh credentials are stored in currently **unencrypted flash/NVS**: keep the board physically trusted. Local HTTP Settings exposes the device sign-in code to LAN interception; enter it only at **auth.openai.com**, never share it or port-forward Settings. Outbound auth/signaling retain verified HTTPS, redirect refusal and certificate checks; WebRTC verifies the peer against the authenticated SDP fingerprint.

This transport is experimental and uses fixed production OpenAI auth/live endpoints. Account/workspace eligibility and subscription limits apply; successful device-code login alone does not prove live voice access. Local remains the default. `CONFIG_UNRAVEL_TTS_CODEX` / `CONFIG_UNRAVEL_TTS_VOICE` only set boot mode/voice; they do not provision an account.


## Talk and typed chat

The native screen owns the tool-capable conversation UUID and bounded context. Physical-window AP chat has independent history/reset, fixed instructions, zero tools and no audio. Text/Codex delegated turns share the native Rust session; live providers retain their own separate connection context. SD resume restores bounded text/tool context, not a live provider session or microphone. Typed chat is for the text-pipeline profile, not native audio.

The shared registry retains timer/Pomodoro, notes/tasks, arithmetic/conversions, Open-Meteo weather, Tavily/Parallel search and board tools, plus `search_history` and three direct Niko tools. Only discovered owner-enabled supported dimmers can be written; generic relays/scenes remain read-only. Niko needs a committed SD ledger before publication, not just a void observer or caller-supplied durable flag. Search has fixed verified endpoints, an eight-second budget, at most four sources and bounded untrusted excerpts; actual source IDs/time/request provenance remain linked to the call, never invented.

Direct utility UI uses `ai_chat_submit_desk_tool` and POD getters without an LLM or microphone. Accepted callbacks run on the worker, not LVGL; handle rejection and copy borrowed replies before GUI updates. Existing local actions retain bounded non-durable protection when SD is unavailable; Niko writes refuse. Timer expiry is acknowledgement-bound and does not start voice/alarm. Notes accessed through AI may enter selected cloud context; direct note controls remain local. SD history/NVS and LAN HTTP are not encrypted; logical deletion is not secure erasure.

### Codex mode

1. Tap the mic to start one live session. Speak successive turns without recording/preview/send steps.
2. Tap again to mute/unmute microphone transmission. Mute discards partial/queued input, but keeps the connection and speaker output running.
3. Hold the native mic to end. Legacy HTTP cannot start, stop or observe native voice.

Incremental provider captions update in place; only canonical final transcripts enter visible history. Mirrored events do not duplicate messages, and identical later utterances remain distinct. Device requests run through the existing backend worker and return their real result to Codex. Mute/Stop remain usable during backend work. New chat, changed speech mode/voice or a committed account change ends the live call. Boot and failures never automatically restart microphone transmission.

### Local mode

Tap Talk to record for at most five seconds, or Stop sooner. The recording stays on-device until Send voice; Discard deletes it. There is no automatic preview playback. Local uses the configured STT service and eSpeak-NG, with recording allocation/I/O and synthesis failures reported. Codex does not silently fall back to Local. Typed read-aloud without an active live call retains the existing one-shot selected-backend speech path.

## Audio and memory

- Codec PCM remains 16000 Hz stereo signed little-endian 16-bit. **Codex** capture encodes 1280-byte/20 ms frames into Opus and decodes provider Opus for checked playback. **Native audio** uses its separately bounded 24 kHz mono PCM/FIR path described above.
- Codex uses eight transmit/eight receive PSRAM packet slots, dropping jitter packets but failing on control pressure. Codec I/O errors end the call.
- Codex peer/capture/playback tasks retain their existing priorities/affinity and pinned 40 KiB encoder/20 KiB decoder PSRAM stacks. Native control uses verified TLS/WebSocket PCM, not Codex WebRTC or the removed LAN bridge.
- The quiet-echo/loud-barge-in heuristic is not acoustic echo cancellation; host conversion/protocol checks do not establish room-audio quality.
- Local recording is capped at 320000 PSRAM bytes. Submission transfers the buffer to the worker without copying/base64, streams multipart WAV to STT, then frees it before inference. External STT and model transport have a 45-second total monotonic deadline, including DNS/connect/upload/headers/body. The shared custom HTTPS transport checks Stop/deadline within underlying I/O loops. Complete STT responses remain capped at 16384 bytes and transcripts at 8192 bytes; invalid/partial/cancelled transcripts are rejected.
- Rust history contains backend text/tool outcomes, never microphone PCM or WAV.
- Local speech uses the embedded English (Great Britain) eSpeak-NG voice/dictionary. It is robotic formant speech, not neural Fish/Kitten speech.
- Synthesis streams 22050 Hz mono PCM through rational interpolation into 16000 Hz stereo 16-bit codec output. Writes contain at most 256 stereo frames (1024 bytes); the full utterance is not buffered.
- eSpeak retains 89,650 bytes (about 87.5 KiB) of dynamically allocated PSRAM workspace. Its CPU-only static phoneme/frame/echo/harmonic workspaces (about 38 KiB across `synthdata`, `synthesize` and `wavegen`) also live in PSRAM through `components/speech_tts/linker.lf`, rather than consuming the internal heap needed during Codex delegation. Assets occupy 827364 flash bytes; code and Unicode data add to that. LVGL objects and TLS buffers also use PSRAM. Model/tool callbacks use serial, fallibly created 12 KiB internal-stack pthreads; the earlier 24 KiB HTTPS stack left >20 KiB unused. Allocation failures are reported instead of rebooting, and callbacks are joined even after an async timeout.
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` is set in the tracked board/default configurations and the flashed local configuration. It also enables ESP-IDF's standard Wi-Fi/lwIP external-BSS mappings. DMA buffers, task-control blocks and network/callback stacks remain internal; codec stack sizes, bounded queues and TLS/fingerprint checks are unchanged. Existing private configurations need this option enabled; adding a default alone does not override an explicit disabled setting.
- Native/legacy text agents share the serial worker and one per-run 128 KiB RPC workspace. Request JSON is freed before callbacks. Native active context checkpoints have 24 KiB/48-message soft limits, preserve complete call/result groups and require archive acknowledgment before eviction; unknown outcomes remain protected. SQLite owns bounded PSRAM allocation/cache and checked `PERSIST`/`FULL` I/O, not raw audio. Storage failure is visible and never triggers format.
- The existing LVGL task is pinned to core 1, away from the core-0 Wi-Fi driver, without another task or stack allocation. Audio capture/playback/network tasks remain separate and keep their current affinity, priorities, stack budgets and bounded queues. A controlled on-board Opus probe is scheduling evidence, not proof of sustained real microphone/duplex performance.
- One-shot Codex read-aloud uses the direct transport without opening microphone capture; decoded output is capped at 150 seconds. Live voice has no configured duration limit, but currently retains only 64 unique delegation IDs and stops at that boundary. This is a known limit, not the established cause of every disconnect.
- Versioned `buddy/tts` NVS stores mode/voice separately from provider/preferences; OAuth credentials use `codex_auth/credentials`. Outbound speech requires verified HTTPS, valid clock and certificate lifetime/hostname/chain checks. Legacy bridge URL/key records are not a setup path for direct voice.

## Interfaces

- `speech_stt_prepare()` before publishing queued work, `speech_stt_cancel()` from native Stop, and `speech_stt_transcribe(pcm, size, url, api_key, model, text, error)` in `components/speech_stt`: synchronous, borrowed PCM, bounded multipart HTTPS, complete validated JSON text. Cancellation never frees a live transport; cleanup remains with the I/O owner.
- `unravel::Agent::run(transcript, model, system_prompt)` in `components/agent_runtime`: actual Rust model/tool loop and canonical conversation session.
- `speech_tts_speak(text, error)` in `components/speech_tts`: synchronous, English-only local synthesis and checked codec playback; call from one worker with recording disabled.
- `speech_live_prepare_speech()` then `speech_live_speak(text, voice, error)` performs serialized direct read-aloud without microphone capture. `speech_live_cancel_speech()` latches cancellation even during authentication.
- `speech_live_start(voice, instructions, callback, context, error)` starts asynchronously using saved Codex credentials. `speech_live_set_muted(bool, error)` gates local transmission; `speech_live_stop()` is nonblocking. `speech_live_context(text, delegation_id, error)` copies/queues bounded context. Callback JSON is valid only during invocation; do not call LVGL or retain pointers. Cleanup precedes the final closed event and subsequent start.
- `codex_auth_begin()`, `codex_auth_cancel()`, `codex_auth_disconnect()` and redacted `codex_auth_status()` implement owner-controlled setup; `codex_auth_credentials(out, cancelled, error)` provides/refreshes credentials internally, never over HTTP.
- `audio_playback_read_chunked_checked()` and `audio_playback_write_checked()` in `components/audio_bsp`: fail on codec initialization/I/O errors. The codec stays at 16000 Hz, two channels, 16 bits.

## Troubleshooting and verification

**Missing STT key / HTTP 401:** Local needs its separate transcription credential; Ollama authentication alone is insufficient. Codex ignores local STT fields. Check **Codex account saved** and the saved Codex mode/voice; if refresh is rejected, reconnect the subscription account in owner Settings. Do not copy tokens or install a host/CLI bridge.

**Saved account, live connection failure:** check Wi-Fi, synchronized clock, outbound HTTPS/WebRTC UDP access and account live eligibility. Signaling now sizes its HTTP transmit buffer for OAuth authorization/account headers and allows 30 seconds; the former fixed-1024-byte header buffer could leave a request incomplete. The mic remains off after failure until explicitly tapped. For audio/context consumer errors, check network/codec progress rather than enlarging bounded queues indefinitely.

**TLS connection failure:** verify Wi-Fi, system clock, hostname and trusted certificate. The clock gate uses synchronized system time rather than consuming SNTP status twice. Keep PSRAM/TLS and 16-socket defaults: the old internal-buffer/10-socket configuration prevented model requests while the browser was connected. Do not disable certificate or hostname verification.

**Clock not synchronized:** the gate allows 45 seconds for cold startup and uses three NTP servers (`time.cloudflare.com`, `time.google.com`, `pool.ntp.org`). A new cold-clock request or Wi-Fi IP event restarts pending synchronization so stale backoff cannot strand the request. Build with `CONFIG_LWIP_SNTP_MAX_SERVERS=3`; valid system time is reused without consuming the one-shot SNTP flag. If all providers fail, the request remains an error—there is no invented fallback date or insecure TLS bypass.

**Screen unexpectedly dark:** Screen sleep Never disables idle blanking, not night dimming. For constant brightness, disable Dim at night as well. With dimming enabled, local hours 22:00–07:00 reduce brightness to roughly one third (minimum 10%); a clock becoming synchronized can therefore suddenly dim the panel. Busy operations suppress idle sleep independently of dimming.

**No microphone / no speech:** check the ES7210/ES8311 initialization logs and speaker connection. Native PCM proof does not establish physical codec output or microphone operation.

### Controlled long-call baseline

The diagnostic firmware emits a fixed `speech_live: metrics` line at start, every ten seconds and after joined transport/media shutdown. Fields include transport phase, elapsed time, internal failure-site label, last numeric peer state, validated WebSocket close code, unique delegation count, sampled TX/RX/event/context queue peaks, current internal/PSRAM free heap, one-second sampled session minima, boot-wide allocator minima, current/minimum sampled largest internal block, and stack high-water marks in bytes (control/peer/mic/speaker). Zero stack headroom in this record means no sample/task, not a measured empty stack. Queue peaks are sampled lower bounds, not lossless occupancy tracing; heap boot minima may predate the call.

No room audio, transcripts, delegation IDs, bearer tokens, OAuth headers or provider error bodies are added to these records. `scripts/collect_voice_baseline.py` observes only this numeric metadata without resetting the board or starting its mic:

```sh
python3 scripts/collect_voice_baseline.py --port /dev/ttyACM0 \
  --seconds 3600 --output /private/path/arvel-voice-baseline.jsonl
```

Use a new output file outside the repository and an isolated Python environment with `pyserial`. Close any other serial monitor first. Explicitly start voice on the physical board or owner-authenticated Home, conduct the controlled conversation and stop it before ending the observation. A collector run with no metrics fails; captured records alone do not prove intelligible audio, interruption quality, a sixty-minute soak or absence of watchdog starvation.

Capture the original physical failure scenario before changing the delegation policy. The existing watchdog evidence in the changelog is a separate unresolved capture/Opus scheduling observation; do not assume removing the delegation ceiling will fix it.


The real authentication-core host suite is `bash components/codex_auth/test/run.sh` (existing C/C++ compilers, pthreads and downloaded cJSON required). Scripted HTTP/NVS/clock fixtures cover sign-in, cancellation, refresh and storage failures; they do not prove real account eligibility, TLS/DNS, ESP32 flash power-loss behavior or physical audio.

The C++/Rust agent regression is `bash components/agent_runtime/test/run.sh` (host Rust stable, C/C++ compilers, pthreads, downloaded cJSON and cached Cargo dependencies required; offline/locked, no installation or provider calls). It runs the real Rust RPC to verify allocation-failure history preservation, retry/continuation and reset. Optional `CFLAGS`, `CXXFLAGS` and `LDFLAGS` support ASan/UBSan.

**Historical bridge-era evidence, not direct-voice verification:** earlier native STT/local synthesis and connected V1 GLM continuation/reset checks exercised real codec writes. Removed bridge versions completed real speech/transcription round-trips, incremental streamed output, two continuous spoken turns and a `get_device_status` delegation; historical suites grew from 17 to 30 to 50 tests. Controlled device transport checks survived a four-second PCM burst and exercised mute/unmute/Stop. Those results concern the old host bridge/WebSocket path, not current phone-authenticated direct WebRTC. They do not establish physical touch/hold, microphone operation, speaker quality or acoustic echo cancellation. No passing direct real-voice or physical verification is claimed here.


The linked speech engine is GPL-3.0-or-later and Rust core AGPL-3.0-only. Root `LICENSE`, component notices and `THIRD_PARTY_NOTICES.md` preserve grants/provenance, but restricted Espressif SDK compatibility and NVIDIA public-use/artwork rights remain unresolved. Current application slots are **two 6 MiB OTA slots, no factory**; this candidate is unflashed. The approved layout preserves NVS/key/SPIFFS addresses but requires the full matching bootloader/table/initial-OTA-metadata/app migration before app-only updates. Do not erase NVS/SD for a voice fix.
