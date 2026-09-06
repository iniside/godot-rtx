#!/usr/bin/env python
"""PreToolUse(Bash) guard enforcing the repository git-safety and Claude
commit-trailer rules:

  1. Git Safety: never discard/overwrite uncommitted work, never reset past
     commits made by other agents, and never rewrite the published fork history.
     Blocks the destructive git verbs the rule names (stash, restore,
     checkout-discard, clean, reset --hard, reset past HEAD~1, filter-branch,
     force-push) plus `rm .git/index`. Bare `git reset` (unstage) and
     `git reset --soft HEAD~1` (undo own just-made commit) stay ALLOWED, as the
     rule explicitly permits.

  2. Commit Message Format: every `git commit` must carry a
     `Co-Authored-By: Claude` trailer so authorship is never silently dropped.

The hook only ever DENIES; it never approves, so non-git Bash and safe git flow
straight through (exit 0 = defer to normal permission handling).
"""
import json
import re
import sys


def deny(reason: str) -> None:
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }))
    sys.exit(0)


def main() -> None:
    try:
        data = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    command = ((data.get("tool_input") or {}).get("command") or "")
    if not command.strip():
        sys.exit(0)

    # Destructive checks scan COMMAND VERBS only — blank out string/here-doc data
    # first so a commit message that merely *describes* a banned verb doesn't
    # trip the guard. The trailer check below deliberately scans the original
    # `command` (it needs the message body).
    code_only = re.sub(r"<<-?\s*['\"]?(\w+)['\"]?.*?\n\1\b", "", command, flags=re.S)
    code_only = re.sub(r"'[^']*'", "''", code_only)
    code_only = re.sub(r'"[^"]*"', '""', code_only)
    norm = re.sub(r"\s+", " ", code_only).strip()

    PREFIX = ".agents/shared/core-rules.md Git Safety (MANDATORY): "

    # --- rm .git/index (but NOT .git/index.lock, a safe stale-lock cleanup) ---
    if re.search(r"\brm\b[^|;&]*\.git/index(?!\.lock)(\b|$|['\"\s])", norm):
        deny(PREFIX + "never hand-edit git internals (`rm .git/index`). "
             "Removing the stale `.git/index.lock` is fine; deleting the index is not.")

    # --- git stash (push/save/bare); list/show/diff are read-only and allowed ---
    if re.search(r"\bgit\s+stash\b", norm) and not re.search(
            r"\bgit\s+stash\s+(list|show|diff)\b", norm):
        deny(PREFIX + "never `git stash` — the tree holds in-progress work across "
             "servers/rendering, drivers and the game projects. To inspect old contents "
             "use `git show <sha>:<path>`.")

    # --- git restore (any form discards worktree/staging) ---
    if re.search(r"\bgit\s+restore\b", norm):
        deny(PREFIX + "never `git restore` — it discards working-tree changes. "
             "Use `git show <sha>:<path>` to inspect old contents.")

    # --- git clean (deletes untracked files) ---
    if re.search(r"\bgit\s+clean\b", norm):
        deny(PREFIX + "never `git clean` — it deletes untracked files, which here means "
             "build output, test scenes and in-progress work.")

    # --- history rewrites ---
    if re.search(r"\bgit\s+filter-branch\b", norm) or re.search(r"\bgit\s+filter-repo\b", norm):
        deny(PREFIX + "never rewrite history — this branch is a published fork of "
             "godotengine/godot and a rewrite destroys the merge bases the next upstream "
             "update needs.")
    if re.search(r"\bgit\s+push\b[^|;&]*(--force\b|--force-with-lease\b|\s-f\b)", norm):
        deny(PREFIX + "never force-push — the fork's published history is what upstream "
             "merges are based on.")

    # --- git checkout that discards files (`-- <path>`, `.`, or `HEAD -- ...`) ---
    # Branch/commit checkout (`git checkout nvidia-pt-dlss`) is allowed.
    if re.search(r"\bgit\s+checkout\b", norm) and (
            re.search(r"\bgit\s+checkout\b[^|;&]*\s--\s", norm) or
            re.search(r"\bgit\s+checkout\s+\.(\s|$)", norm) or
            re.search(r"\bgit\s+checkout\s+HEAD\b", norm)):
        deny(PREFIX + "never `git checkout -- <file>` / `git checkout .` — it overwrites "
             "uncommitted changes. Use `git show <sha>:<path>` to inspect old contents.")

    # --- git reset: block --hard, and any reset past HEAD~1 / to a SHA. ---
    # Allowed: bare `git reset`, `git reset <paths>` (unstage),
    #          `git reset --soft HEAD~1` (undo own just-made commit).
    if re.search(r"\bgit\s+reset\b", norm):
        if re.search(r"\bgit\s+reset\b[^|;&]*--hard\b", norm):
            deny(PREFIX + "never `git reset --hard` — it discards uncommitted changes.")
        if (re.search(r"HEAD~([2-9]|\d{2,})\b", norm) or
                re.search(r"HEAD\^\^", norm) or
                re.search(r"\bgit\s+reset\b(\s+--(soft|mixed|hard))?\s+[0-9a-f]{7,40}\b", norm)):
            deny(PREFIX + "never `git reset` past commits you didn't make this turn — the "
                 "branch carries upstream Godot history and other agents' commits. Only "
                 "`git reset --soft HEAD~1` to undo a commit YOU just made this turn is allowed.")

    # --- git commit must carry a Co-Authored-By: Claude trailer ---
    if re.search(r"\bgit\s+commit\b", norm) and not re.search(
            r"\bgit\s+commit\b[^|;&]*\s(--amend|--no-edit)\b", norm):
        # Check commits whose message is visible in the command: -m / --message, or
        # a here-doc body. `-F <file>` reading an external file is invisible to us,
        # so it is left alone. `norm` had its here-doc blanked, so detect the
        # here-doc on the original command.
        has_message_flag = re.search(r"\bgit\s+commit\b[^|;&]*\s-[a-zA-Z]*m", norm) or \
            re.search(r"\bgit\s+commit\b[^|;&]*--message", norm) or \
            ("<<" in command) or \
            ("Co-Authored-By" in command)
        if has_message_flag and not re.search(r"Co-Authored-By:\s*Claude", command):
            deny(".agents/adapters/claude.md Commit trailer: every commit must end with a "
                 "`Co-Authored-By: Claude <Model> <noreply@anthropic.com>` trailer naming the "
                 "model that ACTUALLY executed the work. Add the trailer and retry.")

    sys.exit(0)


if __name__ == "__main__":
    main()
