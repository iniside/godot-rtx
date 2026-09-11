#!/usr/bin/env python3

import argparse
import json
import math
import os
from pathlib import Path
import sys
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description="Move the first visible Godot editor 3D viewport and restore its camera.")
    parser.add_argument("--project", required=True, type=Path, help="Project directory containing project.godot")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--sequence", type=Path, help="JSON array: duration seconds, translation [x,y,z], rotation_degrees [pitch,yaw]; displacement is relative to each segment's starting camera")
    action.add_argument("--cancel", metavar="ID", help="Cancel the active command with this ID")
    parser.add_argument("--wait", action="store_true", help="Print the terminal result")
    parser.add_argument("--timeout", type=float, default=240, help="Maximum wait in seconds (default 240, maximum 300)")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300 seconds")
    project = args.project.resolve()
    if not (project / "project.godot").is_file():
        parser.error("--project must contain project.godot")
    cache = project / ".godot" / "editor"
    if not cache.is_dir():
        parser.error("Project editor cache is absent; open this project in the editor first")
    request_id = args.cancel or uuid.uuid4().hex
    command = {"id": request_id}
    if args.cancel:
        command["cancel"] = True
    else:
        with args.sequence.open(encoding="utf-8-sig") as file:
            command["sequence"] = json.load(file)
        if not isinstance(command["sequence"], list):
            parser.error("--sequence must contain a JSON array")
        command["expires_unix"] = time.time() + args.timeout
    encoded = json.dumps(command, allow_nan=False)
    if len(encoded.encode("utf-8")) > 16384:
        parser.error("Command exceeds 16384 bytes")
    command_path = cache / "camera_motion.command.json"
    if command_path.exists():
        parser.error("An unconsumed command already exists; wait for the editor to consume it")
    temporary = cache / f"camera_motion.{uuid.uuid4().hex}.tmp"
    try:
        temporary.write_text(encoded, encoding="utf-8")
        os.replace(temporary, command_path)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps({"id": request_id, "status": "submitted", "status_file": str(cache / "camera_motion.status.json")}), flush=True)
    if not args.wait:
        return 0
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            with (cache / "camera_motion.status.json").open(encoding="utf-8") as file:
                status = json.load(file)
        except (OSError, ValueError):
            status = {}
        if isinstance(status, dict) and status.get("id") == request_id and status.get("status") in ("completed", "cancelled", "error"):
            print(json.dumps(status, indent=2, allow_nan=False))
            return 0 if status["status"] in ("completed", "cancelled") else 1
        time.sleep(0.25)
    print(f"Timed out waiting for {request_id}; inspect the status file. If active, cancel with --cancel {request_id} --wait.", file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Camera motion client: {error}", file=sys.stderr)
        sys.exit(1)
