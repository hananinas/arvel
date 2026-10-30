# Released Labs 11B deployment gate

This runner operates **one GPU container on an existing owner-provided Nebius AI Cloud host**. It does not create a VM, install packages, download weights/images, accept licenses, open a microphone, execute home-control actions, or substitute another model. A host-side nginx process supplies authenticated TLS ingress. Run it on the Nebius host, not on Arvel or a remote Docker context.

## Status and legal boundary

The target is released **NVIDIA-NemotronLabs-VoiceChat-11B**, not early-access Nemotron 3 VoiceChat 12B. No GPU deployment, real speech/tool response, driver/runtime compatibility check, session-boundary exercise, or Nebius billing/teardown check has been performed for these scripts. Local input validation is not cloud acceptance.

Published artifact identifiers queried on 2026-10-14 (publication pins, **not GPU-tested pins**):

- NVIDIA image tag `nvcr.io/nim/nvidia/nemotron-labs-voicechat:1.0.0` resolved to `sha256:6e69ff2aac955be2cb65b0de4f5b6d7c0b5e45ca0a1d42a2b153e9b54efb059b` (registry manifest digest and computed SHA256 matched).
- Released HF repository `nvidia/NVIDIA-NemotronLabs-VoiceChat-11B` published revision `443794ea956ef0065f001967ffd00e77f519cb39`.

Independently retain the actual manifest/model-download/build evidence used on your host. The runner accepts only the exact image repository with `@sha256:<64 lowercase hex>`; tags, including `latest`, are rejected. It records image ID, immutable digest, model source/revision, prepared-repository tree hash, profile hash and evidence hashes in private `run.json`. A hash identifies supplied material; it does not establish its authenticity or license rights.

The checkpoint model card declares **OpenMDW-1.1**. The **NGC 1.0.0 container is separately governed by the NVIDIA Software and Model Evaluation License**. The checkpoint license does not grant container permissions. Its February 26, 2025 text includes:

- Section 1: internal test/evaluation only, not production.
- Section 3: restricted authorized internal users, not unrestricted public judging access.
- Section 2.13: no disclosure of evaluation/test results without prior written consent.
- Section 11: no publicity or public information/results without prior written approval.
- Section 13: six-month term from initial download; independently verify continued permission.

**Do not publish a demo, results, container material or unrestricted judging access without the appropriate NVIDIA written permission. Public hackathon use has not been established as permitted.** The owner has chosen to proceed with this documented container target; the runner requires explicit operator acknowledgment and records whether permission is verified/unverified, rather than treating an acknowledgment as public-use approval. A verified claim requires the actual written-permission evidence. Obtain legal clarification before public use; do not silently change models. No script accepts license terms on your behalf.

License: <https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-and-model-evaluation-license/>

## Owner prerequisites

Supply all of the following before `start`:

1. An existing authorized Nebius host/project and explicit spend authorization. Linux x86_64, glibc >=2.35, a supported OS/container runtime, NVIDIA open kernel modules matching a supported driver **major >580**, and one free nominal >=80 GB GPU. The runner conservatively requires >=80000 MiB reported by `nvidia-smi`; the model's approximate 66 GB usage is not a smaller GPU requirement.
2. Owner-installed Docker **>=28**, NVIDIA Container Toolkit/runtime, Python >=3.9, nginx with TLS support, and a functioning **user systemd manager with lingering enabled**. Docker >=28 avoids older loopback-publish LAN exposure. No installation/configuration is automated. Use a local Unix Docker socket and unset `DOCKER_HOST`/`DOCKER_CONTEXT` overrides.
3. The immutable amd64 image already present locally with its matching `RepoDigests`, and a prepared Triton model repository outside this checkout. It must contain `nemotron-voicechat/`, no symlinks/special files, and remain unchanged throughout the run. The mounted model directory is writable because upstream serving/model preparation may need writes; any resulting change requires a newly recorded repository hash before a subsequent start.
4. Either the exact NGC model `nim/nvidia/nemotron-labs-voicechat:1.0.0`, revision `1.0.0`, or a generated repository from `nvidia/NVIDIA-NemotronLabs-VoiceChat-11B` at a full 40-character HF commit. Retain preparation logs, source file hashes and the exact conversion command/image digest. HF conversion does not remove the serving container's license restrictions.
5. A real lowercase DNS hostname pointing to this host, an owner-provided certificate chain covering it, its private key, and a cryptographically random bearer token file. Certificate expiry/hostname/chain validation must pass without insecure flags. For an owner CA, supply its public CA file; never disable verification. Owner-provision the secret through a private file, not a CLI literal, shell history, repository config, image, or chat.
6. Owner-managed firewall/security-group rules exposing only the chosen TLS port to authorized clients (default 8443). No TCP 80 listener, redirect, backend 9000 or Triton 8000/8001/8002 exposure. Do not expose the household device HTTP UI. Other host services/security remain the owner's responsibility.
7. An explicit session verification state. The initial owner-authorized bounded evaluation may start with all session duration/prompt/reset/resume values explicitly unknown. Real serving-profile and boundary-check evidence identifying this image/model are required before claiming verified limits, boundary acceptance or usable rollover; public generic documentation does not supply those limits.

The backend uses exactly `/s2s/run_s2s_server.sh`, `--runtime=nvidia`, one selected GPU, 8 GB shared memory, `/data/models`, and `NIM_HTTP_API_PORT=9000`. Only `127.0.0.1:9000` is published. Triton ports stay unpublished. The private ingress config exposes exactly `GET /v1/realtime` (WebSocket upgrade required) and `GET /v1/realtime/health`, with bearer authentication on both. Alias `/realtime`, service discovery `/`, other paths, query parameters, non-GET requests and redirects are not forwarded. Authorization is stripped before the loopback backend. No access logs, container logs or secret-bearing subprocess output are retained by the runner.

## External serving profile

Create an owner-only JSON file **outside the repository**, mode 0600, containing actual values, not a copied fictitious deployment. Every referenced path must exist outside the checkout. Private key and bearer files must be owner-only/current-user-owned. Keep their parent directories private as well. No runnable profile is included because host authorization, prices and exact deployed limits are not available.

| JSON field | Required actual value |
|---|---|
| `schema_version` | `1` |
| `image` | Exact immutable NVIDIA repository digest described above |
| `model.source` | `ngc` or `huggingface` |
| `model.id`, `model.revision` | Exact source identity/revision from prerequisite 4 |
| `model.repository_sha256` | Output of `hash-model-repo` for the prepared mounted repository |
| `model_directory` | Absolute external prepared-repository directory |
| `hostname` | Lowercase certificate-covered DNS hostname |
| `tls_certificate_file`, `tls_key_file`, `bearer_file` | External actual certificate, private key and one token (32..512 token characters, optional trailing newline) |
| `tls_ca_file` | Optional external public owner-CA file, loaded in addition to system trust |
| `tls_port` | Optional unprivileged TLS port 1024..65535; default 8443 |
| `gpu_index` | Optional `nvidia-smi` GPU index; default 0 |
| `max_run_seconds` | Owner finite service budget 60..86400 seconds, including startup and reserved shutdown time |
| `readiness_timeout_seconds` | Optional positive timeout, default 360; must fit `max_run_seconds - 30` |
| `cost.currency` | Actual three-letter uppercase currency |
| `cost.hourly_total_cost` | Positive actual all-in host/resource hourly estimate in that currency |
| `cost.additional_cost_reserve` | Actual nonnegative allowance for rounded billing, disk/IP/network and other charges |
| `cost.spend_limit` | Explicit positive owner-authorized ceiling; hourly estimate × budget /3600 + reserve must fit |
| `legal.use` | `internal_evaluation` or intended `public_hackathon`; this records intent, not permission |
| `legal.owner_reviewed_and_authorized` | `true` only after owner review and authorization |
| `legal.authorized_users_only` | `true`; restrict credentials/network to license-authorized users |
| `legal.evaluation_restrictions_acknowledged` | `true` after acknowledging internal-use, disclosure and publicity limits |
| `legal.public_permission_status` | `unverified` or `verified`; verified requires actual permission evidence below |
| `session.verification` | `unverified` for initial bounded evaluation, or `verified` with all actual limits and evidence below |
| `session.max_duration_seconds` | `unknown` when unverified; otherwise actual positive server maximum, or `"unlimited"` only if configuration establishes no enforced maximum |
| `session.prompt_limit` | `unknown` when unverified; otherwise object with positive `value` and `unit` (`bytes` or `tokens`) from actual serving profile |
| `session.resume` | `unknown` when unverified; otherwise `not_supported` or `supported` based on real evidence |
| `session.reset_behavior` | `unknown` when unverified; otherwise actual fresh-session/reset behavior description |
| `session.resume_mechanism` | Required only for demonstrated `supported` resume; actual protocol/mechanism |
| `evidence.artifact_provenance` | External nonempty download/build/digest/provenance evidence file |
| `evidence.serving_profile` | Optional during unverified setup; required nonempty actual configuration/runtime evidence when session is verified |
| `evidence.session_boundary_checks` | Optional during unverified setup; required nonempty real boundary observations when session is verified |
| `evidence.platform_support` | External nonempty supported OS/toolkit/driver/open-module evidence |
| `evidence.license_review` | External nonempty owner license-review record |
| `evidence.nebius_authorization` | External nonempty host/project/spend authorization record (no credentials) |
| `evidence.nvidia_written_public_permission` | Actual external written permission, required only when claiming `verified` |

`validate` checks input structure, required material and local path permissions, without Docker, network, resource creation or inference. It cannot verify that an owner's evidence is truthful. `start` additionally hashes the actual model tree, checks host/GPU/runtime/image prerequisites, checks nginx configuration, and exercises readiness. It cannot replace owner legal review or live boundary tests.

### Establishing the session evidence

Before marking the session profile verified, inspect the actual pinned server configuration/runtime and record source locations/hashes. Exercise the configured duration boundary, prompt immediately below/at/above its documented limit, orderly close, server disconnect, and a fresh connection. Record observed terminal events/close codes and whether a new session actually retains anything. If the configuration establishes no duration cap, record that configuration fact plus the finite longest duration exercised; a short successful run alone is not evidence of unlimited duration. Do not invent a resume feature or call a new connection restored acoustic state. For any documented supported resume, exercise that exact mechanism and its expiry/reset boundary. Keep this evidence private and redacted; never retain secrets, household transcripts or microphone audio as deployment logs.

If evidence does not yet exist, use the **same** owner-authorized finite `start` with this honest session object in the otherwise complete external profile:

```json
{
  "verification": "unverified",
  "max_duration_seconds": "unknown",
  "prompt_limit": "unknown",
  "resume": "unknown",
  "reset_behavior": "unknown"
}
```

This is the `session` object, not a complete profile. It permits the bounded initial deployment that discovers actual server behavior; it does not manufacture limits or require an unbounded/manual Docker bypass. Artifact pins, repository hash, legal acknowledgment, owner host access/spend authorization, TLS/auth and independent finite service/cost caps remain mandatory. Supplied optional session evidence is hashed even during bootstrap. The private run record explicitly distinguishes owner-verified limits from unverified setup and always records `rollover_accepted=false`: readiness never proves boundary acceptance or usable rollover. After exercising the same artifact, populate actual limits, supply real serving/boundary evidence, and change `session.verification` to `verified`. The runtime budget is a separate cost-control cap, not a model session limit.

## Commands on the owner host

With `PROFILE`, `MODEL_DIR` and `STATE_DIR` set to the owner's real external paths (path variables, not secret values):

```sh
python3 scripts/voicechat_deploy.py --help
python3 scripts/voicechat_deploy.py validate --help
python3 scripts/voicechat_deploy.py hash-model-repo --model-dir "$MODEL_DIR"
python3 scripts/voicechat_deploy.py validate --profile "$PROFILE"
```

Create an empty state directory with mode 0700. Keep the foreground runner attached:

```sh
python3 scripts/voicechat_deploy.py start --profile "$PROFILE" --state-dir "$STATE_DIR"
```

`run.json` records provenance and readiness without bearer/key content. `nginx.conf` temporarily contains the bearer comparison; it is mode 0600 in the external private state directory and is removed after successful cleanup. Do not commit/share that directory. Do not invoke `nginx -T`, enable request/container logging, or publish evidence without the required permission.

Readiness must be `status=ok`, `mode=triton`, `triton_status=ready` over authenticated hostname-verified TLS. The runner also checks private service discovery rejects demo/loopback mode. A ready service is **not** proof of actual speech, tools, session boundaries or hackathon qualification.

Stop with Ctrl-C, or from a second terminal:

```sh
python3 scripts/voicechat_deploy.py stop --state-dir "$STATE_DIR"
```

Stop addresses only this run's UUID-named systemd services and ownership-labeled container, removes that container, and preserves redacted `run.json`. It does not reload the profile or print the token. Reusing a run requires a new empty state directory.

The finite cap includes startup. Thirty seconds are reserved for shutdown. Independent user-systemd `RuntimeMaxSec` applies to both serving and ingress, and GPU service `ExecStopPost` stops the Docker container even if the foreground runner is killed. No automatic restart is enabled. Ordinary runner exit/error/termination also stops both services and removes the container. If forcibly killed, call `stop` to remove leftover private ingress config/container metadata. A broken Docker daemon/systemd, host suspension or other host failure can defeat timely cleanup; independently monitor the owner cloud budget. Check stop failure messages and manually terminate the service immediately if cleanup fails.

**Container stop is not cloud teardown. Nebius VM/GPU allocation, storage, IP and other billing can persist. Stop/delete the appropriate owner-authorized cloud resources separately and verify their billing state.** Cost arithmetic is a projected bounded service allowance, not a Nebius billing guarantee or permission for unlimited spending. Do not leave the VM running after testing.

## Actual speech/tool acceptance (separate authorized run)

Use the focused file-based smoke client, not NVIDIA's sample handlers that fabricate news/currency data. It does not open a microphone. Obtain explicit inference authorization and owner-consented prerecorded input before invoking it:

```sh
python3 scripts/voicechat_smoke.py --help
```

Supply its `--endpoint wss://<real-host>:<tls-port>/v1/realtime`, `--input-wav`, private `--token-file`, external `--evidence`, actual `--model-revision` and `--image-digest`, and explicit `--allow-inference` per that script's help. Do not paste the bearer into a command. Use the same exact deployment provenance as the runner. Required observations are actual response PCM, final input/output transcripts, and a genuine read-only structured tool request/result. Labs wire audio is **24 kHz mono signed little-endian PCM16, 80 ms/3840-byte chunks in base64 JSON**. Dispatch only complete `response.function_call_arguments.done`; return `conversation.item.create` with `function_call_output` and the same `call_id`. The sample client's 16 kHz input-file example is not the wire contract.

For this smoke client, input WAV must already be **24 kHz mono PCM16**. Pass `--image-digest sha256:<actual digest>` without the registry prefix, and `--model-revision <actual 40-hex HF revision>` or `--model-revision ngc:nim/nvidia/nemotron-labs-voicechat:1.0.0` according to the deployed source. `--validate-only` is offline. See its help for output-file requirements and generated-silence/overall timeout controls.

## Local safe checks and remaining gate

The following are intended parent verification commands; they do not deploy or infer:

```sh
python3 scripts/voicechat_deploy.py --help
python3 scripts/voicechat_deploy.py start --help
python3 scripts/voicechat_deploy.py validate
python3 scripts/voicechat_deploy.py validate --profile /nonexistent/arvel-serving-profile.json
python3 -c 'import ast,pathlib; ast.parse(pathlib.Path("scripts/voicechat_deploy.py").read_text())'
```

The missing-option call should fail through argparse; missing-file validation should return nonzero with an actionable error and no Docker invocation. Also exercise private temporary profiles with real local files: explicit unverified/unknown session bootstrap should validate without serving/boundary files; a verified session must reject missing boundary evidence and validate only with actual values/evidence. Exercise malformed JSON, mutable/tag image, missing session verification state, wrong file permissions, wrong model identity and over-budget authorization. Do not manufacture successful cloud evidence to pass validation.

Still required: owner Nebius host/project/access/quota/spend, installed supported GPU/runtime, actual licensed artifacts, TLS/DNS/token/firewall configuration, owner evidence for the serving limits and boundary checks, live authenticated readiness, real audio/transcript/tool smoke, finite-stop verification (including killed runner), cloud teardown, and appropriate written permission before public use/results/judging. These are unexercised external gates, not successes.

## Upstream references

- Deployment: <https://github.com/NVIDIA-NeMo/Speech/blob/nemotron-labs-voicechat/voicechat_realtime_instructions/deploy.md>
- Prerequisites: <https://github.com/NVIDIA-NeMo/Speech/blob/nemotron-labs-voicechat/voicechat_realtime_instructions/prerequisites.md>
- Protocol: <https://github.com/NVIDIA-NeMo/Speech/blob/nemotron-labs-voicechat/voicechat_realtime_instructions/api-reference.md>
- HF repository preparation: <https://github.com/NVIDIA-NeMo/Speech/blob/nemotron-labs-voicechat/voicechat_realtime_instructions/generate-model-repo.md>
- Released checkpoint: <https://huggingface.co/nvidia/NVIDIA-NemotronLabs-VoiceChat-11B>

Upstream tag-based/plain WebSocket examples are documentation, not permission to use an unpinned image or expose cleartext internet audio. This runner deliberately narrows them.
