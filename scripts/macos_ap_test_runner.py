#!/usr/bin/env python3

import argparse
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request


def run_command(command, check=True):
    proc = subprocess.run(command, capture_output=True, text=True)
    if check and proc.returncode != 0:
        stderr = proc.stderr.strip()
        stdout = proc.stdout.strip()
        message = stderr or stdout or "unknown error"
        raise RuntimeError(f"Command failed ({' '.join(shlex.quote(x) for x in command)}): {message}")
    return proc


def detect_wifi_device():
    proc = run_command(["networksetup", "-listallhardwareports"])
    lines = proc.stdout.splitlines()
    for idx, line in enumerate(lines):
        if line.strip() == "Hardware Port: Wi-Fi":
            for follow in lines[idx + 1 : idx + 5]:
                follow = follow.strip()
                if follow.startswith("Device: "):
                    return follow.split(": ", 1)[1].strip()
    raise RuntimeError("Could not auto-detect Wi-Fi device from networksetup output")


def connect_wifi(device, ssid, password=None):
    command = ["networksetup", "-setairportnetwork", device, ssid]
    if password:
        command.append(password)
    print(f"\n== Connecting Wi-Fi: {device} -> {ssid} ==")
    run_command(command)


def get_current_ssid(device):
    proc = run_command(["networksetup", "-getairportnetwork", device], check=False)
    output = (proc.stdout or proc.stderr or "").strip()
    marker = "Current Wi-Fi Network:"
    if marker in output:
        return output.split(marker, 1)[1].strip()
    if "not associated with an AirPort network" in output:
        return None
    return None


def connect_wifi_with_retry(device, ssid, password=None, attempts=3, settle_seconds=2.0, strict_ssid_check=True):
    last_seen = None
    for attempt in range(1, attempts + 1):
        connect_wifi(device, ssid, password)
        time.sleep(settle_seconds)
        current = get_current_ssid(device)
        last_seen = current
        if current == ssid:
            print(f"Connected to expected SSID: {current}")
            return
        if current is None and not strict_ssid_check:
            print("Could not read current SSID on this macOS build; proceeding without strict SSID verification.")
            return
        print(f"Attempt {attempt}/{attempts}: expected SSID '{ssid}', got '{current}'.")
    raise RuntimeError(f"Failed to join SSID '{ssid}' after {attempts} attempts (last seen: {last_seen})")


def wait_for_http_ready(host, timeout_seconds, probe_interval):
    deadline = time.time() + timeout_seconds
    url = f"http://{host}/api/state"
    last_error = "no response yet"
    print(f"== Waiting for device HTTP readiness: {url} (timeout {timeout_seconds}s) ==")

    while time.time() < deadline:
        try:
            req = urllib.request.Request(url, method="GET")
            with urllib.request.urlopen(req, timeout=2.0) as resp:
                print(f"Device host is reachable (HTTP {resp.status}).")
                return
        except urllib.error.HTTPError as exc:
            # Any HTTP response means the server is up; stop waiting.
            print(f"Device host is reachable (HTTP {exc.code}).")
            return
        except urllib.error.URLError as exc:
            last_error = str(exc)
        except TimeoutError:
            last_error = "connection timed out"
        time.sleep(probe_interval)

    raise RuntimeError(f"Timed out waiting for {host} to become reachable: {last_error}")


def run_phase(script_path, host, mode):
    command = [sys.executable, script_path, "--host", host, "--mode", mode]
    print(f"\n== Running phase: {mode} ==")
    proc = subprocess.run(command)
    if proc.returncode != 0:
        raise RuntimeError(f"Phase failed: {mode}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Connect to prop AP, run HTTP validation phases, then reconnect to home Wi-Fi (macOS)."
    )
    parser.add_argument(
        "--wifi-device",
        default=None,
        help="macOS Wi-Fi device name (example: en0). If omitted, auto-detect from hardware ports.",
    )
    parser.add_argument(
        "--wifi-service",
        dest="wifi_device",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--prop-ssid", required=True, help="Prop AP SSID")
    parser.add_argument("--prop-password", default=None, help="Prop AP password if required")
    parser.add_argument("--host", default="192.168.4.1", help="Prop API host/IP")
    parser.add_argument(
        "--connect-timeout",
        type=int,
        default=35,
        help="Seconds to wait for device host readiness after joining prop AP (default: 35)",
    )
    parser.add_argument(
        "--probe-interval",
        type=float,
        default=1.0,
        help="Seconds between host readiness probes (default: 1.0)",
    )
    parser.add_argument(
        "--connect-attempts",
        type=int,
        default=3,
        help="Number of Wi-Fi association attempts before giving up (default: 3)",
    )
    parser.add_argument(
        "--phases",
        nargs="+",
        choices=["smoke", "commands", "persist-save", "persist-check"],
        default=["smoke", "commands"],
        help="Phases to run from prop_http_smoke_test.py",
    )
    parser.add_argument("--home-ssid", default=None, help="Home SSID to reconnect after test (for example: TMOBILE)")
    parser.add_argument("--home-password", default=None, help="Home SSID password if needed")
    parser.add_argument(
        "--skip-reconnect",
        action="store_true",
        help="Do not reconnect to home SSID at the end",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    script_path = "scripts/prop_http_smoke_test.py"
    wifi_device = args.wifi_device or detect_wifi_device()
    print(f"Using Wi-Fi device: {wifi_device}")

    try:
        connect_wifi_with_retry(
            wifi_device,
            args.prop_ssid,
            args.prop_password,
            attempts=args.connect_attempts,
            strict_ssid_check=False,
        )
        wait_for_http_ready(args.host, args.connect_timeout, args.probe_interval)

        for mode in args.phases:
            run_phase(script_path, args.host, mode)

        if "persist-save" in args.phases and "persist-check" not in args.phases:
            print("\nNOTE: Run a manual reboot before persist-check. The restart command is not a full reboot yet.")

    finally:
        if not args.skip_reconnect and args.home_ssid:
            try:
                connect_wifi_with_retry(
                    wifi_device,
                    args.home_ssid,
                    args.home_password,
                    attempts=args.connect_attempts,
                    strict_ssid_check=False,
                )
                print(f"\nReconnected to {args.home_ssid}.")
            except RuntimeError as exc:
                print(f"WARNING: Could not reconnect to {args.home_ssid}: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())