#!/usr/bin/env python3

import argparse
import json
import sys
import urllib.error
import urllib.request


def request_json(method, url, payload=None, timeout=5.0):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8")
        return resp.status, json.loads(body)


def print_step(title):
    print(f"\n== {title} ==")


def print_json(label, value):
    print(f"{label}: {json.dumps(value, indent=2, sort_keys=True)}")


def ensure(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_smoke(base_url):
    print_step("GET /api/state")
    status, state = request_json("GET", f"{base_url}/api/state")
    ensure(status == 200, f"state returned HTTP {status}")
    ensure("gameState" in state, "state payload missing gameState")
    ensure("timeRemaining" in state, "state payload missing timeRemaining")
    print_json("state", state)

    print_step("GET /api/config")
    status, config = request_json("GET", f"{base_url}/api/config")
    ensure(status == 200, f"config returned HTTP {status}")
    ensure("defaultTime" in config, "config payload missing defaultTime")
    ensure("wireCount" in config, "config payload missing wireCount")
    print_json("config", config)

    print_step("POST /api/command ping")
    status, pong = request_json("POST", f"{base_url}/api/command", {"command": "ping"})
    ensure(status == 200, f"ping returned HTTP {status}")
    ensure(pong.get("event") == "pong", "ping did not return pong event")
    print_json("pong", pong)

    print_step("POST /api/command getState")
    status, state_via_command = request_json("POST", f"{base_url}/api/command", {"command": "getState"})
    ensure(status == 200, f"getState command returned HTTP {status}")
    ensure("gameState" in state_via_command, "getState command missing gameState")
    print_json("state via command", state_via_command)


def run_command_flow(base_url):
    sequence = [
        ("reset", {"command": "reset"}),
        ("start", {"command": "start"}),
        ("pause", {"command": "pause"}),
        ("resume", {"command": "resume"}),
        ("disconnect 1", {"command": "disconnect", "input": 1}),
        ("connect 1", {"command": "connect", "input": 1}),
        ("solve", {"command": "solve"}),
        ("reset", {"command": "reset"}),
        ("start with time", {"command": "start", "time": 120}),
        ("fail", {"command": "fail"}),
        ("reset", {"command": "reset"}),
    ]

    print_step("Command flow")
    for label, payload in sequence:
        status, response = request_json("POST", f"{base_url}/api/command", payload)
        ensure(status == 200, f"{label} returned HTTP {status}")
        print_json(label, response)

    status, final_state = request_json("GET", f"{base_url}/api/state")
    ensure(status == 200, f"final state returned HTTP {status}")
    print_json("final state", final_state)


def run_persist_save(base_url):
    payload = {
        "defaultTime": 222,
        "penalty": 17,
        "maxTries": 4,
        "wireCount": 4,
        "requiredLength": 4,
        "mode": "penalty",
        "lidMode": "ignore",
        "solution": "1234",
        "keepSyncEnabled": False,
        "timeToleranceMs": 1000,
        "dedupeWindowMs": 750,
        "heartbeatInterval": 10,
        "input1Name": "red",
        "input2Name": "green",
        "input3Name": "yellow",
        "input4Name": "blue",
    }

    print_step("POST /api/config/save")
    status, response = request_json("POST", f"{base_url}/api/config/save", payload)
    ensure(status == 200, f"config save returned HTTP {status}")
    ensure(response.get("ok") is True, "config save did not report success")
    print_json("save response", response)
    print("Expected persisted values: defaultTime=222, penalty=17, maxTries=4")


def run_persist_check(base_url):
    print_step("GET /api/config after reboot")
    status, config = request_json("GET", f"{base_url}/api/config")
    ensure(status == 200, f"config after reboot returned HTTP {status}")
    ensure(config.get("defaultTime") == 222, "persist check failed: defaultTime != 222")
    ensure(config.get("penalty") == 17, "persist check failed: penalty != 17")
    ensure(config.get("maxTries") == 4, "persist check failed: maxTries != 4")
    print_json("persisted config", config)


def main():
    parser = argparse.ArgumentParser(description="PX-WiFi-V1 HTTP smoke test")
    parser.add_argument("--host", default="192.168.4.1", help="Device host or IP")
    parser.add_argument(
        "--mode",
        choices=["smoke", "commands", "persist-save", "persist-check", "all"],
        default="all",
        help="Which test phase to run",
    )
    args = parser.parse_args()

    base_url = f"http://{args.host}"

    try:
        if args.mode in ("smoke", "all"):
            run_smoke(base_url)
        if args.mode in ("commands", "all"):
            run_command_flow(base_url)
        if args.mode == "persist-save":
            run_persist_save(base_url)
        elif args.mode == "persist-check":
            run_persist_check(base_url)
        elif args.mode == "all":
            print("\nPersistence save/check is split into two phases.")
            print("Run with --mode persist-save, reboot the device, then run --mode persist-check.")
    except (RuntimeError, urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())