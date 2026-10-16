#!/usr/bin/env python3
"""Owner-authorized Labs 11B realtime smoke; never captures a microphone.

Protocol: NVIDIA-NeMo/Speech, nemotron-labs-voicechat branch,
voicechat_realtime_instructions/api-reference.md and deploy.md.
Requires Python 3.11+; install websockets==15.0.1 in a private virtualenv.
No cloud call occurs with --help or --validate-only.
"""

import argparse
import asyncio
import base64
import binascii
from collections import Counter
from datetime import datetime
import hashlib
import importlib.metadata
import ipaddress
import json
import logging
import math
import os
from pathlib import Path
import re
import ssl
import stat
import sys
import time
from urllib.parse import urlsplit
import uuid
import wave

RATE = 24000
FRAME_SAMPLES = 1920
FRAME_BYTES = 3840
MAX_EVENT = 32768
MAX_TEXT = 8192
MAX_ITEMS = 128
MAX_CALLS = 32
MAX_EVENTS = 20000
MODEL = "nvidia/NVIDIA-NemotronLabs-VoiceChat-11B"
REPO = Path(__file__).resolve().parents[1]
EVENT_TYPES = frozenset({
    "session.created", "session.updated", "session.end", "error",
    "input_audio_buffer.speech_started", "input_audio_buffer.speech_stopped",
    "response.created", "response.output_item.added", "response.content_part.added",
    "response.output_audio.delta", "response.output_audio.done",
    "response.output_audio_transcript.delta", "response.output_audio_transcript.done",
    "response.content_part.done", "response.output_item.done", "response.done",
    "response.function_call_arguments.delta", "response.function_call_arguments.done",
    "conversation.item.input_audio_transcription.delta",
    "conversation.item.input_audio_transcription.completed",
})
TOOL = {
    "name": "get_current_time",
    "description": "Read the current date and time, including timezone, from the client's real system clock.",
    "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
}
INSTRUCTIONS = (
    "You are a native speech assistant. For a spoken question about the current time, "
    "call get_current_time with an empty object, then speak a brief answer using its real result. "
    "Do not invent the time or tool results."
)


class SmokeError(Exception):
    """A deliberately redacted, actionable failure."""


def require(condition, message):
    if not condition:
        raise SmokeError(message)


def strict_json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Protocol JSON has duplicate keys.")
            result[key] = value
        return result

    def constant(_value):
        raise SmokeError("Protocol JSON contains a non-finite number.")

    try:
        return json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, RecursionError):
        raise SmokeError("Protocol JSON is malformed or too deeply nested.") from None


def bounded_text(value, limit=MAX_TEXT):
    require(isinstance(value, str), "Protocol text field is missing or invalid.")
    try:
        require(len(value.encode("utf-8")) <= limit, "Protocol text exceeds its size bound.")
    except UnicodeError:
        raise SmokeError("Protocol text is not valid UTF-8.") from None
    return value


def identity(value):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.:/-]{1,256}", value),
            "Protocol item/call/model identity is missing or invalid.")
    return value


def external_new_file(value):
    path = Path(value).expanduser().resolve()
    require(not path.is_relative_to(REPO), "Evidence and output audio must be outside the repository.")
    require(path.parent.is_dir() and not path.exists(),
            "Output parent must exist and output file must not already exist.")
    return path


def private_token(args):
    if args.token_file:
        path = Path(args.token_file).expanduser().resolve()
        require(not path.is_relative_to(REPO), "Bearer token file must be outside the repository.")
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                    and not info.st_mode & 0o077,
                    "Bearer token file must be owner-owned, regular, and private (chmod 600).")
            raw = stream.read(8193)
        require(len(raw) <= 8192, "Bearer token file is too large.")
        try:
            token = raw.decode("ascii").strip()
        except UnicodeError:
            raise SmokeError("Bearer token file must contain an ASCII token only.") from None
    else:
        token = os.environ.get(args.token_env, "")
    require(re.fullmatch(r"[A-Za-z0-9._~+/-]+=*", token or "") is not None and len(token) <= 8192,
            "Provide a valid bearer token in a private external file or the selected environment variable.")
    return token


def validate(args):
    require(sys.version_info >= (3, 11), "Python 3.11 or newer is required.")
    require(not any(ord(c) <= 32 or ord(c) >= 127 for c in args.endpoint),
            "Endpoint must be an ASCII wss URL without whitespace.")
    url = urlsplit(args.endpoint)
    require(url.scheme == "wss" and url.path == "/v1/realtime" and not url.query
            and not url.fragment and not url.username and not url.password and url.hostname,
            "Use only wss://hostname[:port]/v1/realtime, without credentials/query/fragment.")
    try:
        ipaddress.ip_address(url.hostname)
    except ValueError:
        pass
    else:
        raise SmokeError("Use the certificate's DNS hostname, not an IP address.")
    require(len(url.hostname) <= 253 and all(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
            for label in url.hostname.split(".")), "Endpoint DNS hostname is invalid.")
    port = url.port or 443
    require(1 <= port <= 65535, "Endpoint port is invalid.")
    require(re.fullmatch(r"[0-9a-f]{40}", args.model_revision)
            or args.model_revision == "ngc:nim/nvidia/nemotron-labs-voicechat:1.0.0",
            "Supply the actual deployed HF commit or ngc:nim/nvidia/nemotron-labs-voicechat:1.0.0 version identity.")
    require(re.fullmatch(r"sha256:[0-9a-f]{64}", args.image_digest),
            "Supply the actual deployed container digest, not a tag or invented digest.")
    require(math.isfinite(args.timeout) and 10 <= args.timeout <= 180,
            "Overall timeout must be between 10 and 180 seconds.")
    require(math.isfinite(args.silence_seconds) and 5 <= args.silence_seconds <= 60,
            "Trailing generated silence must be between 5 and 60 seconds.")
    evidence = external_new_file(args.evidence)
    output = external_new_file(args.output_audio) if args.output_audio else None
    require(bool(output) == args.consent_save_audio,
            "Saving speech requires both --output-audio and --consent-save-audio; otherwise audio is discarded.")
    require(output != evidence, "Audio output and evidence paths must be different.")
    with wave.open(args.input_wav, "rb") as source:
        require(source.getcomptype() == "NONE" and source.getnchannels() == 1
                and source.getsampwidth() == 2 and source.getframerate() == RATE,
                "Input must be a RIFF WAV containing 24 kHz mono uncompressed little-endian PCM16.")
        frames = source.getnframes()
        require(0 < frames <= RATE * 60, "Input WAV must contain between one sample and 60 seconds.")
    duration = math.ceil(frames / FRAME_SAMPLES) * 0.08
    silence_frames = math.ceil(args.silence_seconds / 0.08)
    require(args.timeout > duration + silence_frames * 0.08 + 15,
            "Overall timeout must cover paced WAV, trailing silence, setup, and final-event grace (15s).")
    context = ssl.create_default_context(cafile=args.ca_file)
    require(context.check_hostname and context.verify_mode == ssl.CERT_REQUIRED,
            "TLS certificate-chain and hostname validation must remain enabled.")
    return url, port, frames, silence_frames, evidence, output, context


async def health(url, port, token, context):
    """Small bounded HTTP/1.1 reader: no redirects, proxies, or body logging."""
    writer = None
    try:
        async with asyncio.timeout(10):
            reader, writer = await asyncio.open_connection(
                url.hostname, port, ssl=context, server_hostname=url.hostname, limit=16384)
            host = url.hostname + (f":{port}" if port != 443 else "")
            request = (f"GET /v1/realtime/health HTTP/1.1\r\nHost: {host}\r\n"
                       f"Authorization: Bearer {token}\r\nAccept: application/json\r\n"
                       "Accept-Encoding: identity\r\nConnection: close\r\n\r\n")
            writer.write(request.encode("ascii"))
            await writer.drain()
            header = await reader.readuntil(b"\r\n\r\n")
            require(len(header) <= 16384, "Readiness HTTP headers exceed their size bound.")
            rows = header.decode("ascii").split("\r\n")
            require(re.fullmatch(r"HTTP/1\.[01] 200(?: .*|)", rows[0]),
                    "Readiness requires HTTP 200; redirects/auth/degraded responses are rejected.")
            fields = {}
            for row in rows[1:-2]:
                name, sep, value = row.partition(":")
                require(sep and name.lower() not in fields, "Readiness HTTP headers are malformed or repeated.")
                fields[name.lower()] = value.strip()
            require(fields.get("content-encoding", "identity") == "identity",
                    "Compressed readiness responses are not supported.")
            transfer = fields.get("transfer-encoding")
            body = bytearray()
            if transfer:
                require(transfer == "chunked" and "content-length" not in fields,
                        "Readiness HTTP framing is ambiguous or unsupported.")
                while True:
                    line = await reader.readuntil(b"\r\n")
                    require(len(line) <= 128 and re.fullmatch(rb"[0-9A-Fa-f]+\r\n", line),
                            "Readiness chunk header is malformed.")
                    size = int(line.strip(), 16)
                    require(size <= MAX_EVENT - len(body), "Readiness body exceeds 32 KiB.")
                    if not size:
                        require(await reader.readexactly(2) == b"\r\n", "Readiness trailers are unsupported.")
                        break
                    body.extend(await reader.readexactly(size))
                    require(await reader.readexactly(2) == b"\r\n", "Readiness chunk is malformed.")
            elif "content-length" in fields:
                require(re.fullmatch(r"[0-9]{1,6}", fields["content-length"]), "Readiness body length is invalid.")
                size = int(fields["content-length"])
                require(size <= MAX_EVENT, "Readiness body exceeds 32 KiB.")
                body.extend(await reader.readexactly(size))
            else:
                while True:
                    part = await reader.read(min(4096, MAX_EVENT + 1 - len(body)))
                    if not part:
                        break
                    body.extend(part)
                    require(len(body) <= MAX_EVENT, "Readiness body exceeds 32 KiB.")
            result = strict_json(body)
            require(isinstance(result, dict) and result.get("status") == "ok"
                    and result.get("mode") == "triton" and result.get("triton_status") == "ready",
                    "Readiness must report status=ok, mode=triton, triton_status=ready; loopback is rejected.")
    finally:
        if writer:
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 1)
            except (TimeoutError, OSError, ssl.SSLError):
                pass


class Observations:
    def __init__(self):
        self.start = time.monotonic()
        self.counts = Counter()
        self.timings = {}
        self.server_model = None
        self.users = {}
        self.assistant = {}
        self.calls = {}
        self.tool_results = 0
        self.audio_bytes = 0
        self.input_nonzero = False

    def mark(self, name):
        self.timings.setdefault(name, round(time.monotonic() - self.start, 3))

    def count(self, kind):
        self.counts[kind if kind in EVENT_TYPES else "other"] += 1
        require(sum(self.counts.values()) <= MAX_EVENTS, "Server event count exceeds its safety bound.")

    def item(self, event):
        key = (identity(event.get("response_id")), identity(event.get("item_id")))
        if key not in self.assistant:
            require(len(self.assistant) < MAX_ITEMS, "Assistant item count exceeds its safety bound.")
            self.assistant[key] = {"spoken": False, "final": False, "audio_done": False, "after_tool": False}
        return self.assistant[key]

    def passed(self):
        return self.input_nonzero and bool(self.users) and bool(self.calls) and self.tool_results > 0 and any(
            item["spoken"] and item["final"] and item["audio_done"] and item["after_tool"]
            for item in self.assistant.values())

    def evidence(self, args, status, failure):
        return {
            "schema": 1, "status": status, "failure": failure,
            "target_model": MODEL,
            "owner_declared_deployed_model_revision": args.model_revision,
            "owner_declared_deployed_image_digest": args.image_digest,
            "server_session_model": self.server_model,
            "identity_note": "Owner pins are not independently GPU-verified by this client; session model is server-reported.",
            "event_counts": dict(sorted(self.counts.items())),
            "counts": {"user_final_items": len(self.users),
                       "assistant_final_items": sum(item["final"] for item in self.assistant.values()),
                       "read_only_tool_executions": len(self.calls),
                       "tool_results_sent": self.tool_results,
                       "output_pcm_samples": self.audio_bytes // 2},
            "timings_seconds": {**self.timings, "elapsed": round(time.monotonic() - self.start, 3)},
            "readiness": "triton_ready" if "health_ready" in self.timings else "not_verified",
            "audio_saved_by_explicit_consent": bool(args.output_audio),
            "limitations": "Returned PCM/nonempty final transcripts are protocol evidence, not a listening-quality or firmware/hardware test.",
        }


async def send(ws, kind, **fields):
    await ws.send(json.dumps({"type": kind, "event_id": str(uuid.uuid4()), **fields}, separators=(",", ":")))


def verify_formats(session):
    require(isinstance(session, dict), "Server session configuration is missing.")
    audio = session.get("audio")
    require(isinstance(audio, dict), "Server audio configuration is missing.")
    for direction in ("input", "output"):
        settings = audio.get(direction)
        require(isinstance(settings, dict) and settings.get("format") == {"type": "audio/pcm", "rate": RATE},
                "Server did not acknowledge the supported 24 kHz PCM audio format.")


async def receive_event(ws, observed):
    raw = await ws.recv()
    require(isinstance(raw, str) and len(raw.encode("utf-8")) <= MAX_EVENT,
            "Server must send JSON text events bounded to 32 KiB.")
    event = strict_json(raw)
    require(isinstance(event, dict) and isinstance(event.get("type"), str), "Server event envelope is invalid.")
    observed.count(event["type"])
    require(event["type"] != "error", "Server reported an inference/protocol error; private error body withheld.")
    require(event["type"] != "session.end", "Server ended the session before the smoke completed.")
    return event


async def handle_event(ws, event, observed, audio_output):
    kind = event["type"]
    if kind == "response.function_call_arguments.done":
        call_id = identity(event.get("call_id"))
        name = bounded_text(event.get("name"), 128)
        arguments = strict_json(bounded_text(event.get("arguments")))
        require(name == "get_current_time" and isinstance(arguments, dict) and not arguments,
                "Only get_current_time with validated empty object arguments is permitted; conflicting/unsupported calls fail.")
        canonical = (name, json.dumps(arguments, sort_keys=True))
        if call_id in observed.calls:
            stored, result = observed.calls[call_id]
            require(stored == canonical, "Repeated tool call ID has conflicting arguments.")
        else:
            require(len(observed.calls) < MAX_CALLS, "Tool call count exceeds its safety bound.")
            result = json.dumps({"time": datetime.now().astimezone().isoformat(timespec="seconds")})
            observed.calls[call_id] = (canonical, result)
            observed.mark("first_read_only_tool")
        await send(ws, "conversation.item.create", item={
            "type": "function_call_output", "call_id": call_id, "output": result})
        observed.tool_results += 1
        observed.mark("first_tool_result_sent")
    elif kind == "response.output_audio.delta":
        item = observed.item(event)
        encoded = bounded_text(event.get("delta"), MAX_EVENT)
        try:
            pcm = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            raise SmokeError("Output audio is malformed base64.") from None
        require(pcm and len(pcm) % 2 == 0, "Output audio must contain aligned PCM16 samples.")
        observed.audio_bytes += len(pcm)
        require(observed.audio_bytes <= RATE * 2 * 120, "Output audio exceeds the 120-second safety bound.")
        nonzero = any(pcm)
        item["spoken"] |= nonzero
        item["after_tool"] |= nonzero and observed.tool_results > 0
        if nonzero:
            observed.mark("first_nonzero_output_audio")
        if audio_output:
            audio_output.writeframesraw(pcm)
    elif kind == "response.output_audio.done":
        observed.item(event)["audio_done"] = True
    elif kind == "response.output_audio_transcript.done":
        text = bounded_text(event.get("transcript"))
        if text.strip():
            observed.item(event)["final"] = True
            observed.mark("first_assistant_final")
    elif kind == "conversation.item.input_audio_transcription.completed":
        item_id = identity(event.get("item_id"))
        text = bounded_text(event.get("transcript"))
        if text.strip():
            digest = hashlib.sha256(text.encode("utf-8")).digest()
            if item_id in observed.users:
                require(observed.users[item_id] == digest, "Repeated final user item has conflicting text.")
            else:
                require(len(observed.users) < MAX_ITEMS, "User item count exceeds its safety bound.")
                observed.users[item_id] = digest
            observed.mark("first_user_final")
    elif kind in {"response.output_audio_transcript.delta", "conversation.item.input_audio_transcription.delta",
                  "response.function_call_arguments.delta"}:
        bounded_text(event.get("delta"))  # Never dispatch partial arguments or retain partial transcripts.


async def stream_input(ws, source, frames, silence_frames, observed):
    for _ in range(math.ceil(frames / FRAME_SAMPLES)):
        pcm = source.readframes(FRAME_SAMPLES)
        require(pcm and len(pcm) % 2 == 0, "Input WAV is truncated or malformed.")
        observed.input_nonzero |= any(pcm)
        pcm = pcm.ljust(FRAME_BYTES, b"\x00")
        await send(ws, "input_audio_buffer.append", audio=base64.b64encode(pcm).decode("ascii"))
        await asyncio.sleep(0.08)
    require(source.tell() == frames, "Input WAV does not contain its declared samples.")
    observed.mark("wav_sent")
    silence = base64.b64encode(bytes(FRAME_BYTES)).decode("ascii")
    for _ in range(silence_frames):
        await send(ws, "input_audio_buffer.append", audio=silence)
        await asyncio.sleep(0.08)
    observed.mark("trailing_silence_sent")
    await asyncio.sleep(5)  # Allow already-produced final events to drain; no unsupported commit/response.create.


async def conversation(args, validated, token, observed, audio_output):
    from websockets.asyncio.client import connect

    class NoRedirectConnect(connect):
        def process_redirect(self, _exc):
            return SmokeError("WebSocket handshake failed; redirects are forbidden. Check auth/TLS/endpoint.")

    url, port, frames, silence_frames, _evidence, _output, context = validated
    async with asyncio.timeout(args.timeout):
        await health(url, port, token, context)
        observed.mark("health_ready")
        async with NoRedirectConnect(
            args.endpoint, ssl=context, additional_headers={"Authorization": f"Bearer {token}"},
            proxy=None, compression=None, max_size=MAX_EVENT, max_queue=4,
            open_timeout=10, close_timeout=2, ping_interval=20, ping_timeout=10,
        ) as ws:
            event = await asyncio.wait_for(receive_event(ws, observed), 10)
            require(event["type"] == "session.created", "Expected session.created before configuration.")
            verify_formats(event.get("session"))
            require(event["session"].get("model") == "nemotron-voicechat",
                    "Server session did not identify the documented nemotron-voicechat model.")
            observed.server_model = "nemotron-voicechat"
            observed.mark("session_created")
            await send(ws, "session.update", session={
                "audio": {"input": {"format": {"type": "audio/pcm", "rate": RATE}},
                          "output": {"format": {"type": "audio/pcm", "rate": RATE}}},
                "instructions": INSTRUCTIONS, "tools": [TOOL],
            })
            event = await asyncio.wait_for(receive_event(ws, observed), 10)
            require(event["type"] == "session.updated", "Expected session.updated acknowledgment before input audio.")
            session = event.get("session")
            verify_formats(session)
            tools = session.get("tools")
            if isinstance(tools, str):
                tools = strict_json(bounded_text(tools))
            require(isinstance(tools, list) and len(tools) == 1 and isinstance(tools[0], dict)
                    and tools[0].get("name") == TOOL["name"], "Server did not acknowledge the read-only tool configuration.")
            observed.mark("session_updated")

            async def consume():
                while True:
                    await handle_event(ws, await receive_event(ws, observed), observed, audio_output)

            with wave.open(args.input_wav, "rb") as source:
                tasks = [asyncio.create_task(stream_input(ws, source, frames, silence_frames, observed)),
                         asyncio.create_task(consume())]
                try:
                    done, _pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                    for task in done:
                        task.result()
                finally:
                    for task in tasks:
                        task.cancel()
                    await asyncio.gather(*tasks, return_exceptions=True)
            require(observed.passed(),
                    "Smoke incomplete: require non-silent input, final user/assistant transcripts, a real time-tool call, "
                    "and nonzero completed output audio after its result. Check the spoken request and silence duration.")
            observed.mark("acceptance_events_received")
            await send(ws, "session.close")
            # No session-resume or session-duration claim is inferred from this short run.


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Record a real request such as 'What is the current time? Use the time tool.' No microphone, playback, "
               "transcript log, or alternate model is used. --allow-inference confirms owner authority, license permission "
               "for internal evaluation, and paid-inference spend authorization. It does not authorize public results. "
               "The NVIDIA evaluation container's public hackathon/judging permission must be resolved separately. "
               "Evidence is redacted but still evaluation results; do not publish without necessary written permission.")
    result.add_argument("--endpoint", required=True, help="Owner ingress wss://DNS-host[:port]/v1/realtime only")
    result.add_argument("--input-wav", required=True, help="Real owner-provided 24 kHz mono PCM16 WAV, <=60 seconds")
    result.add_argument("--model-revision", required=True,
                        help="Actual deployed HF 40hex commit, or ngc:nim/nvidia/nemotron-labs-voicechat:1.0.0")
    result.add_argument("--image-digest", required=True, help="Actual deployed image sha256 digest (not proof of GPU testing)")
    result.add_argument("--evidence", required=True, help="New private JSON evidence file outside the repository")
    secret = result.add_mutually_exclusive_group()
    secret.add_argument("--token-file", help="Private owner-owned external file containing only bearer token")
    secret.add_argument("--token-env", default="VOICECHAT_BEARER_TOKEN", help="Token environment-variable name, not token literal")
    result.add_argument("--ca-file", help="Optional owner CA PEM; certificate/hostname checks cannot be disabled")
    result.add_argument("--silence-seconds", type=float, default=20, help="Generated trailing silence, 5..60 seconds (default 20)")
    result.add_argument("--timeout", type=float, default=120, help="Overall deadline, 10..180 seconds (default 120)")
    result.add_argument("--output-audio", help="Explicit new external WAV path; discarded by default, never played")
    result.add_argument("--consent-save-audio", action="store_true", help="Explicit consent to save model speech at --output-audio")
    result.add_argument("--allow-inference", action="store_true", help="Explicit authorization for this real, potentially paid evaluation")
    result.add_argument("--validate-only", action="store_true", help="Validate local URL/WAV/pins/paths/TLS settings; no network/secrets/writes")
    return result


def main():
    args = parser().parse_args()
    observed = Observations()
    output_handle = audio_output = None
    validated = None
    attempted = False
    status, failure = "failed", None
    try:
        validated = validate(args)
        if args.validate_only:
            print("Local inputs valid. No network, inference, secret read, evidence write, or audio capture occurred.")
            return 0
        require(args.allow_inference, "No inference authorized. Use --validate-only, or supply --allow-inference after obtaining owner/license/spend permission.")
        require(importlib.metadata.version("websockets") == "15.0.1",
                "Use websockets==15.0.1 in a private virtualenv; its redirect/proxy behavior is explicitly constrained here.")
        logging.getLogger("websockets").setLevel(logging.CRITICAL)
        token = private_token(args)
        attempted = True
        if validated[5]:
            fd = os.open(validated[5], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            output_handle = os.fdopen(fd, "wb")
            audio_output = wave.open(output_handle, "wb")
            audio_output.setnchannels(1)
            audio_output.setsampwidth(2)
            audio_output.setframerate(RATE)
        asyncio.run(conversation(args, validated, token, observed, audio_output))
        status = "passed"
    except SmokeError as exc:
        failure = str(exc)  # Only our static, redacted messages; never remote exception text.
    except importlib.metadata.PackageNotFoundError:
        failure = "Missing dependency: install websockets==15.0.1 in a private virtualenv (not system-wide)."
    except TimeoutError:
        failure = "Readiness/setup/overall deadline expired; inspect private serving configuration and endpoint readiness."
    except (OSError, ValueError, wave.Error, EOFError, ssl.SSLError, UnicodeError):
        failure = "Local file, WAV, TLS, or network operation failed; check paths/permissions, format, hostname, CA and ingress. Details withheld."
    except KeyboardInterrupt:
        failure = "Owner interrupted the smoke; no automatic restart."
    except Exception:
        failure = "Realtime transport/protocol operation failed. Check private ingress/server logs; remote details withheld."
    finally:
        try:
            if audio_output:
                audio_output.close()
        except (OSError, wave.Error):
            status, failure = "failed", "Could not finalize the consented output WAV; output may be partial."
        finally:
            if output_handle:
                try:
                    output_handle.close()
                except OSError:
                    status, failure = "failed", "Could not close the consented output WAV; output may be partial."
    if validated and attempted:
        try:
            fd = os.open(validated[4], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as evidence:
                json.dump(observed.evidence(args, status, failure), evidence, indent=2)
                evidence.write("\n")
        except OSError:
            status, failure = "failed", "Could not create the private evidence file; run is not recorded as passed."
    if status == "passed":
        print("PASS: real native protocol audio, final user/assistant transcripts and read-only time tool observed. Private redacted evidence written.")
        return 0
    print("FAIL: " + (failure or "Incomplete smoke."), file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
