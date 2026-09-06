#!/usr/bin/env python
"""PostToolUse(Bash): remind after a successful git commit to review an
above-threshold task in a fresh context.

The reminder does not skip Markdown or other prose: workflow and policy files
can be above the shared dispatch threshold and need the same classification.
It is non-blocking; AGENTS.md and planning-dispatch.md remain authoritative.
"""
import json
import re
import sys

COMMIT_LINE = re.compile(r"^\[[^\]]*?\b([0-9a-f]{7,40})\]", re.MULTILINE)


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 0

    command = ((data.get("tool_input") or {}).get("command") or "").lower()
    if "git commit" not in command or "commit-graph" in command:
        return 0
    if "--dry-run" in command:
        return 0

    response = data.get("tool_response") or {}
    commit_sha = ""
    if isinstance(response, dict):
        combined = str(response.get("stdout", "")) + str(response.get("stderr", ""))
        if "nothing to commit" in combined.lower():
            return 0
        matches = COMMIT_LINE.findall(str(response.get("stdout", "")))
        if matches:
            commit_sha = matches[-1]

    target = commit_sha or "<resolve-current-commit-sha-now>"

    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": (
                "HOSTILE DIFF REVIEW: classify the completed task against the "
                "shared inline threshold. If it is above threshold, dispatch a "
                "FRESH hostile-reviewer in a separate context. Capture the exact "
                f"commit now: review git show {target} and the cumulative diff "
                f"from the task-start commit to {target}; do not review a later "
                "moving branch tip or the author's summary. Policy/Markdown "
                "files are not automatically skipped. If executable tests, "
                "fixtures, or verification changed, also dispatch proof-auditor. "
                "Maximum two fresh review rounds per task."
            ),
        }
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
