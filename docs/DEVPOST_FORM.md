# Devpost form draft — Arvel

Paste-ready field text below. Bracketed items are unresolved submission inputs, not completed work. This draft uses the recorded local research and host verification; no device, provider or GPU was contacted to prepare it. Recheck current event rules before submitting.

## Project name

Arvel

## Short tagline

An ESP32 desk companion with owner-controlled tools and SD conversation history.

## Description: features, functionality and architecture

Arvel is a desk companion for people who want a small touchscreen and voice interface for desk tasks, conversation history and explicitly authorized home controls. It runs on the Waveshare ESP32-S3-Touch-LCD-3.49. The board handles the touchscreen, microphone/speaker lifecycle, device tools and local storage; a separately configured cloud service handles conversation inference. No Nemotron model runs on the ESP32.

The ESP-IDF firmware integrates a Rust agent runtime through an in-process C ABI and C++ model/tool callbacks. Sixteen registered tools cover timers, notes, calculation, unit conversion, weather, web search, device/battery/Wi-Fi status, display/sound/profile settings, history search and three Niko Home Control II operations. Weather, search, inference and Niko need their respective external services or controller; a registered tool is not evidence that those services have been exercised.

An authenticated SvelteKit owner web app is compiled into the firmware. It exposes settings, conversation/tool states, returned source links and history management. Web voice controls operate the board microphone, not the browser microphone. SQLite on the owner's SD card stores finalized text and tool evidence, not raw audio by default. Resuming restores bounded text context; it does not reopen voice or repeat an earlier action. The durable tool ledger reuses completed results, rejects conflicting identities/arguments and reports unfinished calls as unknown after recovery. It is not an exactly-once guarantee across physical power loss.

The Niko integration uses direct MQTT over verified TLS. Writes require an allowlisted supported light/dimmer, confirmation and ready durable storage. The default allowlist is empty. A published command is not proof of a physical effect, and uncertain actions are not automatically replayed. There is no device shell or generic command-execution tool.

### Exactly where NVIDIA and Nebius fit

**Submission inference deployment: [TBD: Nebius Token Factory endpoint + Nemotron model - pending owner setup]**

This is a pending deployment choice, not a configured or verified inference service. For a compatible text model, Nebius Token Factory would perform remote text inference through its OpenAI-compatible API. That does not establish native speech support. The separate native-audio adapter targets the released NVIDIA-NemotronLabs-VoiceChat-11B protocol, with the model served on owner-provisioned Nebius AI Cloud GPU compute if that route is selected and authorized. In that architecture, NVIDIA's model would consume speech and generate speech/tool events in the cloud, while the ESP32 would stream audio, execute permitted tools and maintain local state/history.

The actual endpoint, exact model artifact, transport, permissions and judging availability must be established before claiming NVIDIA-on-Nebius operation. Token Factory and a self-hosted VoiceChat server are not interchangeable protocols. No live Token Factory call, Nebius GPU deployment or NVIDIA speech/tool inference has been performed in these submission-preparation rounds. The existing text pipeline, experimental Codex subscription voice and native-audio adapter are explicit alternatives; an unavailable service does not silently switch to another model. Codex is not evidence of qualifying NVIDIA inference.

### What has been verified

Local frontend checks, behavioral tests, production web builds, ESP-IDF firmware builds and host integration/storage suites have passed as recorded in `docs/submission-verification.md`. The storage regression checks 80 read-only delegation identities across archive changes and reopen, including cached results, conflicts and unknown-on-recovery. Production Codex voice still has its separate 64-delegation ceiling. Host results do not establish physical audio, interruption, SD power-loss behavior, household effects, cloud latency or a long-call soak.

Owner HTTP is LAN-only and not encrypted; flash/NVS credentials and SD history are also unencrypted. Configured cloud services receive the selected conversation/audio/tool context. Combined-firmware distribution compatibility, NVIDIA container/public-demo/judging permission and artwork/footage rights remain unresolved release gates. We will not present these local results as hardware/cloud acceptance or publish private firmware binaries as submission attachments.

## Demo URL

[needs decision]

Physical AI exempts the public hosted-demo URL under the recorded rules. The proposed demonstration is an owner-operated physical device with coordinated judge testing, not a publicly exposed household HTTP interface. Confirm the current exemption and agree the access arrangement with organizers before submission. No hosted demo is claimed here.

## YouTube link

[TBD after filming]

The planned public English-language video is under three minutes. `docs/demo-video-storyboard.md` is the shot plan, not captured footage. Rights, consent and genuine supported NVIDIA-on-Nebius inference must be established before filming the qualifying run.

## Track

**Physical AI.** Arvel is a physical sensing/acting device with a microphone, speaker, touchscreen and bounded device/home-control tools, rather than only a browser chatbot.

The planned continuous 0:15–1:40 segment provides 85 seconds of actual hardware operation: owner-started microphone, a genuine NVIDIA-on-Nebius spoken exchange, one confirmed allowlisted lighting action with its physical effect shown, interruption and a real search answer with its returned source. These are planned acceptance shots, not completed demonstrations. If a dependency is unavailable, disclose the failure; do not substitute fixtures, cached audio, Codex or a health response for qualifying hardware/model operation. Restart/SD recall is a separately labeled edit, not proof of an uninterrupted soak.

## Tool feedback

### ESP-IDF and Espressif Installation Manager (EIM)

The firmware builds with ESP-IDF 5.5.3 and the installed Xtensa Rust toolchain. The stock `export.sh` instructions did not match this host's EIM installation. The round-1 record captured these exact diagnostics:

```text
ERROR: ESP-IDF Python virtual environment "/home/hanan/.espressif/python_env/idf5.5_py3.12_env/bin/python" not found.
ERROR: /home/hanan/.espressif/espidf.constraints.v5.5.txt doesn't exist.
```

Selecting the EIM tool root with the stock exporter also reported installed compiler/debugger tools missing. That exporter returned zero while `idf.py --version` returned 127. The working path was EIM's generated `activate_idf_v5.5.3.sh`, followed by an explicit Bash function invoking `"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"`; its interactive alias is not expanded by noninteractive Bash.

In the read-only build sandbox, EIM's optional IDE-selection sync returned 1 even though the environment, version command and builds worked. Chaining the function definition on activation success initially produced this exact automation error:

```text
bash: line 1: idf.py: command not found
```

Separate core environment activation from optional IDE sync, document the EIM noninteractive invocation and make activation failures propagate consistently. These are observations about this installed layout, not a general claim that the conventional installer is broken. We did not reinstall or rearrange the toolchains, and did not rerun the already-recorded stock-export failure in round 3.

### models.dev catalog integration

The recorded research fetch returned HTTP 200, CORS `*` and 229 providers. The `nebius` entry identified Nebius Token Factory at `https://api.tokenfactory.nebius.com/v1`, with OpenAI-compatible text transport. There was no Nebius VoiceChat entry in that response. Separately, the NVIDIA `nvidia/nemotron-voicechat` entry described audio input but text-only output, contradicting the linked NVIDIA model card's speech-output description. The Nebius speech catalog inspected during that research also did not list VoiceChat; that does not prove private hosted access is unavailable.

We integrated browser-side discovery with explicit supported transports and a custom/self-hosted deployment option. Catalog modality metadata is useful for selection, but cannot establish a realtime wire protocol or working endpoint. More precise versioned model identities, output modalities and protocol references would prevent a catalog selection being mistaken for deployment readiness. These are prior research findings, not a fresh catalog/provider query in this round.

### NVIDIA VoiceChat-11B deployment research

The proposed native-audio target is the released `NVIDIA-NemotronLabs-VoiceChat-11B`, not the separately described early-access Nemotron 3 VoiceChat 12B experience. The researched serving configuration requires x86_64 Linux and an 80 GB VRAM-class NVIDIA GPU; approximately 66 GB reported model usage is not a smaller supported GPU requirement. It uses a Triton-backed serving container and a prepared model repository. Container readiness can take about five minutes according to the serving material; this is not an Arvel-measured startup or latency result.

The public realtime instructions provide the deployment and speech/function-call protocol. The adapter targets 24 kHz mono PCM16 speech and explicit WebSocket events; a Token Factory text endpoint cannot be presumed to speak that protocol. Exact model revision/container digest, compatible drivers/runtime, quota and spend authorization, TLS/DNS/bearer ingress, authenticated readiness, real audio/tool behavior and session duration/reset limits still need live validation. The model checkpoint license does not establish permission to use or publicly demonstrate the serving container.

A single versioned matrix linking checkpoint, container, driver requirements, realtime schema and session limits would make integration easier. We have researched these prerequisites and implemented local protocol/deployment tooling; we have not run this GPU/container deployment and cannot provide measured throughput, latency, audio quality, cloud reliability or cost feedback.

### Nebius usage status

No live Token Factory call has been made yet. No Nebius GPU resource has been provisioned or exercised during these rounds. The catalog entry and configurable endpoint establish an integration path, not service usage. Deployment access, GPU quota, bounded spending and qualifying NVIDIA inference remain pending; there is no invented provider success, latency or billing measurement in this form.

### Research references

- models.dev catalog: https://models.dev/api.json
- Token Factory quickstart: https://docs.tokenfactory.nebius.com/quickstart
- Nebius speech catalog: https://forge.nebius.cloud/models/#catalog-group-speech
- Released model: https://huggingface.co/nvidia/NVIDIA-NemotronLabs-VoiceChat-11B
- Native deployment/protocol instructions: https://github.com/NVIDIA-NeMo/Speech/tree/nemotron-labs-voicechat/voicechat_realtime_instructions
- Separate linked NVIDIA model card: https://build.nvidia.com/nvidia/nemotron-voicechat/modelcard
- Event rules: https://nebiusglobalaihackathon.devpost.com/rules

## Existed before submission period

Arvel was not built from scratch for this submission. The 2026-10-16 commit below is the pre-submission firmware/web baseline. The later entries identify submission-preparation changes rather than claiming the existing board/runtime integration was new event work. The dated table is reproduced from `docs/SUBMISSION.md`.

| Date | Commit | Change |
| --- | --- | --- |
| 2026-10-16 | `db0c837591b0edda06d221efb7c224931073cc94` | Arvel desk-companion firmware and web-interface baseline |
| 2026-10-19 | `3d1e8998960ee91d952d8d7309b30ede1482569f` | Round 1: align Codex auth host transport fixture with current constructor contract |
| 2026-10-19 | `255fa83cd57c870d7403140edcc40d58141672d8` | Round 2: keyboard-accessible transcript focus link and ephemeral Vite test listeners; retained dependency warning documented |
| 2026-10-19 | `e77db06f09127376883dd7d6461ea75741fec2fa` | Round 2: exercised EIM/build/host instructions and committed verification evidence |
| 2026-10-19 | `2026b872cda24693f7c4a4cb94b8e64e8bdba8db` | Round 2: one 80-ID durable read-only delegation ledger regression; no production speech_live changes |
| 2026-10-19 | `50e7b221300e29c995ea594ac73f7ee77d5d7bd4` | Round 2: judge submission draft and timed 2:50 demo storyboard |
| 2026-10-19 | `8fea697a7911d37a6f8eb81bc30d289d95decfd8` | Round 3: regenerate and embed fixed owner UI assets |
| 2026-10-19 | This commit: `docs(submission): finalize round-3 evidence and Devpost form` | Round 3: build measurements, final gate evidence and paste-ready Devpost form with honest deployment/access placeholders |

The final row names this introducing documentation commit by subject; its SHA is available from the committed repository history rather than a self-referential hash in the file.

## Judging access plan: December 1–15, 2026

Proposed arrangement, not yet booked or provisioned: keep an owner-owned device and a reproducible, judge-testable build available for the recorded judging window, December 1 through December 15, 2026 at 12:00 Pacific. Confirm the current rules and organizer requirements, and arrange free access to the submitted project for judges throughout that window without granting access to the household network or unrelated accounts.

Use an isolated demonstration network, synthetic/non-private conversation history, a disposable project-scoped owner account and only dedicated, explicitly allowlisted demonstration hardware. Coordinate hands-on access or an organizer-approved remote testing session with the owner. Do not expose the unencrypted household HTTP API publicly or treat a view-only video as a substitute for required testing access. Provide the exact source/build revision, build/run instructions, disclosed limitations and a reset procedure that cannot replay physical actions. Confirm license/distribution permissions before handing over a firmware image.

If cloud inference is established, provide dedicated restricted project credentials through a private channel, never household credentials or personal provider accounts. Restrict them to the demonstration deployment and required judging capabilities; keep internal Triton ports private. The owner must fund and authorize the necessary availability, with monitored spend limits and an agreed testing schedule that still satisfies judging access. Resolve any availability/cost conflict before submission rather than claiming an unfunded always-on service.

Keep Stop/microphone control, action confirmation and the empty-by-default allowlist intact. Revoke temporary accounts/tokens, stop/delete authorized cloud resources and remove demonstration data after the window. No judging account, deployment or organizer arrangement has been established by this draft.
