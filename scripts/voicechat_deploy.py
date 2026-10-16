#!/usr/bin/env python3
"""Owner-operated, finite VoiceChat service on an existing Nebius host."""
import argparse
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import ssl
import stat
import subprocess
import sys
import time
import uuid
from decimal import Decimal, InvalidOperation

ROOT = Path(__file__).resolve().parents[1]
IMAGE = re.compile(r"nvcr\.io/nim/nvidia/nemotron-labs-voicechat@sha256:[0-9a-f]{64}\Z")
LABEL = "io.arvel.voicechat.run"
WARNING = "WARNING: stopping containers does NOT stop Nebius VM, disk, IP or other cloud billing. Stop/delete resources separately."


class GateError(Exception):
    pass


def need(condition, message):
    if not condition:
        raise GateError(message)


def external(value, kind="file", private=False):
    need(isinstance(value, str) and value, "Required external path is missing")
    p = Path(value).expanduser().resolve()
    need(not p.is_relative_to(ROOT), "Inputs/state/evidence must be outside the repository")
    need(p.is_dir() if kind == "directory" else p.is_file(), "Required external " + kind + " is missing")
    if private:
        s = p.stat()
        need(s.st_uid == os.getuid() and not s.st_mode & 0o077,
             "Private inputs must belong to the current user and have no group/other permissions")
    return p


def private_directory(value):
    p = external(value, "directory", True)
    need(not any(p.iterdir()), "Start requires an empty private state directory; use stop on an existing run")
    return p


def read_json(path):
    need(path.stat().st_size <= 1024 * 1024, "JSON input exceeds 1 MiB")
    try:
        return json.loads(path.read_text())
    except (ValueError, UnicodeError):
        raise GateError("Invalid JSON input (contents suppressed)") from None


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def repository_hash(path):
    """Hash canonical [relative POSIX filename, content SHA256] rows, no symlinks."""
    rows = []
    for p in sorted(path.rglob("*")):
        need(not p.is_symlink(), "Model repository must not contain symlinks")
        if p.is_file():
            rows.append([p.relative_to(path).as_posix(), sha(p)])
        else:
            need(p.is_dir(), "Model repository contains a special file")
    need(rows, "Model repository is empty")
    return hashlib.sha256(json.dumps(rows, ensure_ascii=True, separators=(",", ":")).encode()).hexdigest()


def integer(value, low, high, field):
    need(type(value) is int and low <= value <= high, "Invalid or missing " + field)
    return value


def text(value, field):
    need(isinstance(value, str) and value.strip() and value.lower() not in {"unknown", "todo", "tbd"},
         "Missing real evidence for " + field)
    return value


def validate(profile_path):
    path = external(profile_path, private=True)
    p = read_json(path)
    need(isinstance(p, dict) and p.get("schema_version") == 1, "Expected profile schema_version 1")
    need(IMAGE.fullmatch(p.get("image", "")), "image must be the exact NVIDIA VoiceChat repository at an immutable sha256 digest")
    m = p.get("model", {})
    need(isinstance(m, dict), "Missing model provenance")
    if m.get("source") == "ngc":
        need(m.get("id") == "nim/nvidia/nemotron-labs-voicechat:1.0.0" and m.get("revision") == "1.0.0",
             "NGC model must be exact released 1.0.0 identity")
    elif m.get("source") == "huggingface":
        need(m.get("id") == "nvidia/NVIDIA-NemotronLabs-VoiceChat-11B" and
             re.fullmatch(r"[0-9a-f]{40}", m.get("revision", "")), "HF model needs the released 11B repository and full immutable commit")
    else:
        raise GateError("model.source must be ngc or huggingface; early-access 12B is not this target")
    need(re.fullmatch(r"[0-9a-f]{64}", m.get("repository_sha256", "")), "Missing prepared model repository tree SHA256")
    model_dir = external(p.get("model_directory"), "directory")
    need((model_dir / "nemotron-voicechat").is_dir(), "Expected prepared Triton repository containing nemotron-voicechat")
    hostname = p.get("hostname", "")
    need(isinstance(hostname, str) and hostname == hostname.lower() and len(hostname) <= 253 and "." in hostname and
         all(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", x) for x in hostname.split(".")),
         "hostname must be a real DNS name covered by the TLS certificate")
    integer(p.get("tls_port", 8443), 1024, 65535, "tls_port (unprivileged TLS listener)")
    integer(p.get("gpu_index", 0), 0, 1024, "gpu_index")
    integer(p.get("max_run_seconds"), 60, 86400, "max_run_seconds (60..86400)")
    integer(p.get("readiness_timeout_seconds", 360), 1, p["max_run_seconds"] - 30, "readiness_timeout_seconds")
    paths = {k: external(p.get(k), private=k in {"tls_key_file", "bearer_file"})
             for k in ("tls_certificate_file", "tls_key_file", "bearer_file")}
    need(paths["bearer_file"].stat().st_size <= 1024, "Bearer file is too large")
    token = paths["bearer_file"].read_text().rstrip("\r\n")
    need(re.fullmatch(r"[A-Za-z0-9._~+/\-]{32,512}={0,2}", token), "Bearer file must contain one strong token, at least 32 characters")
    legal = p.get("legal", {})
    need(isinstance(legal, dict) and legal.get("owner_reviewed_and_authorized") is True,
         "Owner must independently review and authorize licenses; the runner does not accept them")
    use = legal.get("use")
    need(use in {"internal_evaluation", "public_hackathon"}, "legal.use must be internal_evaluation or public_hackathon")
    need(legal.get("authorized_users_only") is True, "Access must be restricted to license-authorized users")
    records = p.get("evidence", {})
    need(isinstance(records, dict), "Missing evidence file references")
    required = ["artifact_provenance", "platform_support", "license_review", "nebius_authorization"]
    need(legal.get("evaluation_restrictions_acknowledged") is True,
         "Operator must explicitly acknowledge internal-use, disclosure and publicity restrictions")
    need(legal.get("public_permission_status") in {"verified", "unverified"},
         "Record public permission as verified or unverified; acknowledgment is not permission")
    if legal["public_permission_status"] == "verified":
        required.append("nvidia_written_public_permission")
    limits = p.get("session", {})
    need(isinstance(limits, dict) and limits.get("verification") in {"verified", "unverified"},
         "session.verification must explicitly be verified or unverified")
    if limits["verification"] == "verified":
        required.extend(["serving_profile", "session_boundary_checks"])
        duration = limits.get("max_duration_seconds")
        if duration != "unlimited":
            integer(duration, 1, 2147483647, "session.max_duration_seconds")
        prompt = limits.get("prompt_limit", {})
        need(isinstance(prompt, dict) and prompt.get("unit") in {"bytes", "tokens"}, "Verified prompt limit needs bytes or tokens unit")
        integer(prompt.get("value"), 1, 2147483647, "session.prompt_limit.value")
        need(limits.get("resume") in {"not_supported", "supported"}, "Session resume must be explicitly evidenced, not assumed")
        text(limits.get("reset_behavior"), "session.reset_behavior")
        if limits["resume"] == "supported":
            text(limits.get("resume_mechanism"), "session.resume_mechanism")
    else:
        need(all(limits.get(k) == "unknown" for k in
                 ("max_duration_seconds", "prompt_limit", "resume", "reset_behavior")),
             "Unverified bootstrap profile must explicitly mark duration/prompt/resume/reset unknown; no guessed limits")
        required.extend(k for k in ("serving_profile", "session_boundary_checks") if records.get(k))
    evidence = {k: external(records.get(k)) for k in required}
    need(all(f.stat().st_size > 0 for f in evidence.values()), "Evidence files must not be empty")
    cost = p.get("cost", {})
    need(isinstance(cost, dict) and re.fullmatch(r"[A-Z]{3}", cost.get("currency", "")), "Cost currency must be explicit")
    try:
        hourly = Decimal(str(cost.get("hourly_total_cost")))
        limit = Decimal(str(cost.get("spend_limit")))
        fixed = Decimal(str(cost.get("additional_cost_reserve")))
        need(all(x.is_finite() for x in (hourly, limit, fixed)) and hourly > 0 and limit > 0 and fixed >= 0,
             "Cost inputs must be finite, hourly cost/spend limit positive, reserve nonnegative")
        need(hourly * Decimal(p["max_run_seconds"]) / Decimal(3600) + fixed <= limit,
             "Finite service budget exceeds owner spend authorization")
    except InvalidOperation:
        raise GateError("Missing numeric cost/spend authorization") from None
    return path, p, paths, token, evidence, model_dir


def command(args, timeout=30, capture=False):
    try:
        r = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise GateError("Required host command failed or timed out: " + Path(args[0]).name) from None
    need(r.returncode == 0, "Host command failed (output suppressed): " + Path(args[0]).name)
    return r.stdout.decode() if capture else ""


def host_checks(p):
    need(platform.system() == "Linux" and platform.machine() == "x86_64", "Host must be x86_64 Linux")
    libc, version = platform.libc_ver()
    need(libc == "glibc" and tuple(int(x) for x in version.split(".")[:2]) >= (2, 35), "Host requires glibc >=2.35")
    tools = {k: shutil.which(k) for k in ("docker", "nginx", "nvidia-smi", "systemd-run", "systemctl", "loginctl")}
    need(all(tools.values()), "Owner must install/configure Docker, nginx, NVIDIA runtime and user systemd; nothing is installed automatically")
    need(command([tools["loginctl"], "show-user", str(os.getuid()), "--property=Linger", "--value"], capture=True).strip() == "yes",
         "Owner must enable user-systemd lingering so finite-stop services survive logout")
    command([tools["systemctl"], "--user", "show-environment"])
    gpu = command([tools["nvidia-smi"], "--query-gpu=index,memory.total,driver_version", "--format=csv,noheader,nounits"], capture=True)
    selected = [x.split(",") for x in gpu.splitlines() if x.split(",")[0].strip() == str(p.get("gpu_index", 0))]
    need(len(selected) == 1, "Selected GPU is unavailable")
    row = [x.strip() for x in selected[0]]
    need(len(row) == 3 and float(row[1]) >= 80000, "Selected GPU must have nominal >=80GB VRAM (>=80000 MiB reported)")
    need(int(row[2].split(".")[0]) > 580, "NVIDIA-supported driver major must be >580")
    info = json.loads(command([tools["docker"], "info", "--format", "{{json .}}"], capture=True))
    need("nvidia" in info.get("Runtimes", {}), "Docker NVIDIA runtime is not configured")
    need(info.get("OSType") == "linux" and info.get("Architecture") in {"x86_64", "amd64"}, "Docker daemon must be local x86_64 Linux")
    need(int(info.get("ServerVersion", "0").split(".")[0]) >= 28,
         "Docker >=28 required: older loopback-published ports can be reachable on the local network")
    # Force the local socket: remote Docker would invalidate loopback and systemd ownership.
    need(not os.environ.get("DOCKER_HOST") and not os.environ.get("DOCKER_CONTEXT"), "Unset Docker host/context overrides; a local daemon is required")
    endpoint = json.loads(command([tools["docker"], "context", "inspect"], capture=True))[0]["Endpoints"]["docker"]["Host"]
    need(endpoint.startswith("unix:///"), "Docker context must use a local Unix socket")
    image = json.loads(command([tools["docker"], "image", "inspect", p["image"]], capture=True))[0]
    need(p["image"] in image.get("RepoDigests", []) and image.get("Architecture") == "amd64", "Owner must preload exact digest-pinned amd64 image")
    return tools, {"gpu_memory_mib": row[1], "driver_version": row[2], "image_id": image["Id"]}


def quote(value):
    need(not any(c in str(value) for c in ("\n", "\r", "\x00", "$")), "Unsafe nginx path")
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def ingress_config(p, paths, token, state):
    port = p.get("tls_port", 8443)
    # No redirects, access logs, other paths, plain HTTP listeners, forwarded credentials or aliases.
    return f'''pid {quote(state / "nginx.pid")};
error_log /dev/null;
worker_processes 1;
events {{ worker_connections 128; }}
http {{
  access_log off;
  server_tokens off;
  client_max_body_size 64k;
  client_body_temp_path {quote(state / "client_temp")};
  proxy_temp_path {quote(state / "proxy_temp")};
  proxy_intercept_errors on;
  proxy_hide_header Location;
  error_page 301 302 303 307 308 =502 @rejected_redirect;
  server {{
    listen {port} ssl default_server;
    server_name {p["hostname"]};
    ssl_certificate {quote(paths["tls_certificate_file"])};
    ssl_certificate_key {quote(paths["tls_key_file"])};
    ssl_protocols TLSv1.2 TLSv1.3;
    if ($host != {p["hostname"]}) {{ return 403; }}
    if ($http_authorization != "Bearer {token}") {{ return 401; }}
    if ($args != "") {{ return 400; }}
    location @rejected_redirect {{ return 502; }}
    location = /v1/realtime {{
      if ($request_method != GET) {{ return 405; }}
      if ($http_upgrade != "websocket") {{ return 400; }}
      proxy_pass http://127.0.0.1:9000;
      proxy_http_version 1.1;
      proxy_set_header Host localhost;
      proxy_set_header Authorization "";
      proxy_set_header Upgrade websocket;
      proxy_set_header Connection upgrade;
      proxy_buffering off;
      proxy_read_timeout {p["max_run_seconds"]}s;
      proxy_send_timeout 30s;
    }}
    location = /v1/realtime/health {{
      if ($request_method != GET) {{ return 405; }}
      proxy_pass http://127.0.0.1:9000;
      proxy_set_header Host localhost;
      proxy_set_header Authorization "";
      proxy_set_header Connection "";
      proxy_connect_timeout 5s;
      proxy_read_timeout 5s;
    }}
    location / {{ return 404; }}
  }}
}}
'''


def health(host, port, token=None, context=None, path="/v1/realtime/health"):
    conn = http.client.HTTPSConnection(host, port, timeout=5, context=context) if context else http.client.HTTPConnection(host, port, timeout=5)
    try:
        conn.request("GET", path, headers={"Authorization": "Bearer " + token} if token else {})
        r = conn.getresponse()
        body = r.read(65537)
        need(r.status == 200 and len(body) <= 65536, "Readiness response rejected; redirects are never followed")
        data = json.loads(body)
        need(isinstance(data, dict), "Invalid readiness response")
        if path == "/":
            need(data.get("loopback_mode") is False and data.get("model_name") == "nemotron-voicechat", "Demo/loopback or unexpected serving identity rejected")
        else:
            need(data.get("status") == "ok" and data.get("mode") == "triton" and data.get("triton_status") == "ready",
                 "Real Triton-backed inference is not ready")
        return data
    finally:
        conn.close()


def save(path, data):
    with path.open("w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    path.chmod(0o600)


def stop(state):
    s = read_json(state / "run.json")
    need(isinstance(s, dict), "Invalid run state JSON")
    run = s.get("run_id", "")
    need(re.fullmatch(r"[0-9a-f]{32}", run), "Invalid run identity")
    docker = shutil.which("docker")
    systemctl = shutil.which("systemctl")
    need(docker and systemctl, "Stop requires Docker and user systemd")
    failures = []
    for unit in (f"arvel-voicechat-ingress-{run}", f"arvel-voicechat-gpu-{run}"):
        try:
            load = command([systemctl, "--user", "show", unit, "--property=LoadState", "--value"], capture=True).strip()
            if load != "not-found":
                command([systemctl, "--user", "stop", unit], timeout=45)
        except GateError:
            failures.append("systemd service stop failed")
    name = "arvel-voicechat-" + run
    try:
        inspect = subprocess.run([docker, "inspect", "--format", "{{json .Config.Labels}}", name],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10, check=False)
        if inspect.returncode == 0:
            need(json.loads(inspect.stdout).get(LABEL) == run, "Container ownership label mismatch; refusing stop")
            command([docker, "stop", "--time", "10", name], timeout=30)
            command([docker, "rm", name])
        else:
            # An absent container is success only if Docker itself is still reachable.
            command([docker, "info"])
    except (GateError, subprocess.TimeoutExpired, ValueError):
        failures.append("GPU container cleanup failed")
    if not failures:
        (state / "nginx.conf").unlink(missing_ok=True)
        s["stopped_at_unix"] = time.time()
        save(state / "run.json", s)
    need(not failures, "; ".join(failures) + "; manually stop the named services/container immediately")


def start(args):
    path, p, paths, token, files, model = validate(args.profile)
    if p["session"]["verification"] == "unverified":
        print("Bounded bootstrap evaluation: session duration/prompt/reset/resume are UNKNOWN; rollover is NOT accepted.", file=sys.stderr)
    state = private_directory(args.state_dir)
    tools, runtime = host_checks(p)
    need(repository_hash(model) == p["model"]["repository_sha256"], "Prepared model repository does not match provenance hash")
    # Owner controls model files; do not mutate them while serving.
    run = uuid.uuid4().hex
    name = "arvel-voicechat-" + run
    gpu_unit = "arvel-voicechat-gpu-" + run
    ingress_unit = "arvel-voicechat-ingress-" + run
    budget = p["max_run_seconds"] - 30  # Reserve shutdown time within owner duration/cost cap.
    record = {"schema_version": 1, "run_id": run, "started_at_unix": time.time(), "profile_sha256": sha(path),
              "image": p["image"], "model": p["model"], "session": p["session"], "cost": p["cost"],
              "max_run_seconds": p["max_run_seconds"], "endpoint": f'wss://{p["hostname"]}:{p.get("tls_port", 8443)}/v1/realtime',
              "runtime": runtime, "legal": p["legal"], "evidence_sha256": {k: sha(f) for k, f in files.items()},
              "verification": "Not speech/tool/boundary acceptance; readiness will be recorded separately"}
    record["session_limits_owner_verified"] = p["session"]["verification"] == "verified"
    record["rollover_accepted"] = False
    save(state / "run.json", record)
    config = state / "nginx.conf"
    config.write_text(ingress_config(p, paths, token, state))
    config.chmod(0o600)
    deadline = time.monotonic() + budget
    common = ["--user", "--quiet", "--property=StandardOutput=null", "--property=StandardError=null", "--property=TimeoutStopSec=20s"]
    old = {}
    def interrupted(signum, frame):
        raise GateError("Explicit stop/termination received")
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        old[sig] = signal.signal(sig, interrupted)
    try:
        command([tools["nginx"], "-e", "/dev/null", "-t", "-p", str(state) + "/", "-c", str(config)])
        command([tools["docker"], "create", "--name", name, "--label", LABEL + "=" + run,
                 "--pull=never", "--restart=no", "--log-driver=none", "--runtime=nvidia", "--gpus", "device=" + str(p.get("gpu_index", 0)),
                 "--shm-size=8GB", "--env", "NIM_HTTP_API_PORT=9000", "--publish", "127.0.0.1:9000:9000",
                 "--mount", "type=bind,source=" + str(model) + ",target=/data/models",
                 "--entrypoint", "/s2s/run_s2s_server.sh", p["image"]])
        remaining = math.floor(deadline - time.monotonic())
        need(remaining > 0, "Service budget exhausted before launch")
        command([tools["systemd-run"], *common, "--unit=" + gpu_unit,
                 "--property=RuntimeMaxSec=" + str(remaining),
                 "--property=ExecStopPost=" + tools["docker"] + " stop --time 10 " + name,
                 "--", tools["docker"], "start", "--attach", name])
        command([tools["systemd-run"], *common, "--unit=" + ingress_unit,
                 "--property=RuntimeMaxSec=" + str(max(1, math.floor(deadline - time.monotonic()))),
                 "--", tools["nginx"], "-e", "/dev/null", "-p", str(state) + "/", "-c", str(config), "-g", "daemon off;"])
        context = ssl.create_default_context()
        if p.get("tls_ca_file"):
            context.load_verify_locations(str(external(p["tls_ca_file"])))
        ready_deadline = min(deadline, time.monotonic() + p.get("readiness_timeout_seconds", 360))
        ready = False
        while time.monotonic() < ready_deadline:
            try:
                health("127.0.0.1", 9000, path="/")
                health("127.0.0.1", 9000)
                result = health(p["hostname"], p.get("tls_port", 8443), token, context)
                ready = True
                break
            except (GateError, OSError, ValueError, http.client.HTTPException):
                time.sleep(2)
        need(ready, "Readiness failed: verify TLS/DNS/auth and real Triton configuration privately; no raw response is logged")
        record["authenticated_triton_readiness_at_unix"] = time.time()
        record["readiness"] = {k: result[k] for k in ("status", "mode", "triton_status")}
        save(state / "run.json", record)
        print("Authenticated Triton readiness passed. Speech/tool and boundary acceptance are not asserted.")
        print("Endpoint: " + record["endpoint"])
        print("Stop: python3 scripts/voicechat_deploy.py stop --state-dir " + str(state))
        while time.monotonic() < deadline:
            for unit in (gpu_unit, ingress_unit):
                command([tools["systemctl"], "--user", "is-active", "--quiet", unit])
            time.sleep(min(2, max(0, deadline - time.monotonic())))
        print("Finite service budget reached; shutting down.")
    finally:
        # A second termination cannot interrupt cleanup; systemd also enforces the independent cap.
        for sig in old:
            signal.signal(sig, signal.SIG_IGN)
        try:
            stop(state)
        finally:
            for sig, handler in old.items():
                signal.signal(sig, handler)
            print(WARNING, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=WARNING)
    sub = parser.add_subparsers(dest="operation", required=True)
    for op in ("validate", "start"):
        q = sub.add_parser(op, help="Validate owner evidence locally" if op == "validate" else "Run finite services on this existing host")
        q.add_argument("--profile", required=True, help="Private external JSON profile; see VOICECHAT_DEPLOYMENT.md")
        if op == "start":
            q.add_argument("--state-dir", required=True, help="Empty external owner-only directory")
    q = sub.add_parser("stop", help="Stop the specific owned run without reloading secrets/profile")
    q.add_argument("--state-dir", required=True)
    q = sub.add_parser("hash-model-repo", help="Compute prepared model tree hash without Docker/network")
    q.add_argument("--model-dir", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        if args.operation == "validate":
            validate(args.profile)
            print("Owner-input structure validated locally; evidence authenticity, platform, GPU, cloud, licenses and live boundaries are NOT verified.")
        elif args.operation == "hash-model-repo":
            print(repository_hash(external(args.model_dir, "directory")))
        elif args.operation == "stop":
            try:
                stop(external(args.state_dir, "directory", True))
            finally:
                print(WARNING, file=sys.stderr)
        else:
            start(args)
        return 0
    except (GateError, OSError, ValueError, KeyError, TypeError, ssl.SSLError):
        # Never echo raw config, backend errors, nginx config or secret-bearing subprocess output.
        exc = sys.exc_info()[1]
        print("ERROR: " + str(exc) if isinstance(exc, GateError) else "ERROR: Invalid input or host operation failed (details suppressed)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
