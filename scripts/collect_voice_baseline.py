#!/usr/bin/env python3
"""Observe fixed voice metrics only; never reset the board or activate capture."""
import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path

PHASES = {'authentication', 'peer_offer', 'signaling', 'peer_channel', 'sideband_upgrade', 'ready'}
FIELDS = {
    'final': 1, 'elapsed_ms': 1, 'peer': 1, 'ws_close': 1, 'delegations': 1,
    'queue_sample_hwm': 4, 'free_internal': 1, 'free_psram': 1,
    'session_sample_min': 2, 'boot_min': 2, 'largest_internal': 1,
    'largest_sample_min': 1, 'stack_min_bytes': 4,
}


def parse_metrics(line):
    """Extract only bounded numeric metadata, ignoring all ordinary serial logs."""
    marker = 'speech_live: metrics '
    if marker not in line or len(line) > 2048:
        return None
    tail = line.split(marker, 1)[1]
    pairs = tail.split()
    if len(pairs) != len(FIELDS) + 2:
        return None
    result = {}
    for pair in pairs:
        key, separator, value = pair.partition('=')
        if not separator or key in result:
            return None
        if key == 'phase':
            if value not in PHASES:
                return None
            result[key] = value
        elif key == 'site':
            # Internal static site labels only, never provider error bodies.
            if not re.fullmatch(r'[a-z_]{1,32}', value):
                return None
            result[key] = value
        elif key in FIELDS:
            parts = value.split('/')
            if len(parts) != FIELDS[key] or any(not re.fullmatch(r'-?[0-9]{1,20}', p) for p in parts):
                return None
            numbers = [int(p) for p in parts]
            if any(n < (-1 if key == 'peer' else 0) or n > 2**64 - 1 for n in numbers):
                return None
            result[key] = numbers[0] if len(numbers) == 1 else numbers
        else:
            return None
    return result if result.keys() == FIELDS.keys() | {'phase', 'site'} else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Connected USB serial port; opened without changing DTR/RTS')
    parser.add_argument('--seconds', type=int, default=3600, help='Observation duration, 1..7200 seconds')
    parser.add_argument('--output', type=Path, required=True, help='New private JSONL evidence file, outside repository')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 7200:
        parser.error('--seconds must be 1..7200')
    repository = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    if output.is_relative_to(repository):
        parser.error('Evidence must be stored outside the repository')
    try:
        import serial
    except ImportError:
        parser.error('Install pyserial in an isolated Python environment')
    count = 0
    started = time.monotonic()
    maximum = args.seconds + 128
    # exclusive prevents accidentally competing with an active IDF monitor.
    port = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with output.open('x', encoding='utf-8') as evidence:
            output.chmod(0o600)
            evidence.write(json.dumps({'kind': 'observation', 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'duration_seconds': args.seconds, 'mic_activated_by_collector': False}) + '\n')
            port.open()
            pending = bytearray()
            overlong = False
            while time.monotonic() - started < args.seconds:
                data = port.read(256)
                for byte in data:
                    if byte == 10:
                        record = None if overlong else parse_metrics(pending.decode('ascii', errors='replace'))
                        pending.clear()
                        overlong = False
                        if record is not None:
                            if count >= maximum:
                                raise ValueError('Diagnostic record bound reached; observation stopped')
                            record['observed_ms'] = int((time.monotonic() - started) * 1000)
                            evidence.write(json.dumps(record) + '\n')
                            evidence.flush()
                            count += 1
                    elif not overlong:
                        if len(pending) < 2048:
                            pending.append(byte)
                        else:
                            # Discard the entire overlong line, not just a prefix.
                            pending.clear()
                            overlong = True
            evidence.write(json.dumps({'kind': 'observation_end', 'metrics_records': count, 'observed_seconds': round(time.monotonic() - started, 3)}) + '\n')
    finally:
        port.close()
    if not count:
        print('No live metrics observed. This is not a passing baseline; use the diagnostic firmware and explicitly start voice on Arvel.', file=sys.stderr)
        return 2
    print(f'Captured {count} redacted metrics records; no transcripts, tokens or audio recorded. Physical conversation/soak acceptance still requires reviewing the run.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f'Baseline collection failed: {type(error).__name__}; check the serial port and output file.', file=sys.stderr)
        sys.exit(1)
