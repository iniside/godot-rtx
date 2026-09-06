"""PreToolUse hook: inject a once-per-session reminder to read the relevant
reference doc / run the matching closure step, based on the tool input.

Triggers (path-based, Edit/Write; command-based, Bash/PowerShell):
  - thirdparty/ or a *.gen.* file    -> godot-rules.md Upstream Fork Hygiene
  - _bind_methods / scene / servers  -> taxonomy Class 1 (bind + doc/classes XML + register)
  - *.glsl                           -> taxonomy Class 3 (SCsub entry, shader/C++ define parity)
  - renderer_rd / rendering_device   -> taxonomy Class 3 + Class 2 (barriers, RID lifetime)
  - drivers/                         -> taxonomy Class 3/5 (per-backend parity)
  - SConstruct / SCsub / detect.py   -> taxonomy Class 5 (targets, #ifdef sets)
  - editor/                          -> taxonomy Class 5 (TOOLS_ENABLED / template build)
  - a scons invocation               -> godot-rules.md build authority

Each doc is reminded at most once per session (marker files keyed by
session_id + doc, under %TEMP%/claude/godot-rtx-ref-hook). Silent (exit 0, no
stdout) when there is nothing fresh to surface.
"""
import sys
import json
import os
import re

TAXONOMY = "docs/reference/godot-failure-taxonomy.md"
GODOT_RULES = ".agents/shared/godot-rules.md"


def docs_for_path(norm):
    docs = []
    if "/thirdparty/" in norm or norm.startswith("thirdparty/") or ".gen." in norm:
        docs.append(GODOT_RULES + " (Upstream Fork Hygiene — thirdparty/ and *.gen.* "
                    "are never hand-edited; change the source or record a patch)")
    if norm.endswith(".glsl"):
        docs.append(TAXONOMY + " (Class 3 — a new .glsl needs its SCsub entry; keep C++ "
                    "and GLSL defines/variants in sync)")
    if "/renderer_rd/" in norm or "rendering_device" in norm or "rendering_device_graph" in norm:
        docs.append(TAXONOMY + " (Class 3 — resource state & barriers, uniform-set/shader "
                    "layout parity; Class 2 — RID free on every exit path)")
    if norm.startswith("drivers/") or "/drivers/" in norm:
        docs.append(TAXONOMY + " (Class 3/5 — does the sibling backend, D3D12 vs Vulkan vs "
                    "Metal, need the same change?)")
    if norm.startswith("scene/") or norm.startswith("servers/") or norm.startswith("core/"):
        docs.append(TAXONOMY + " (Class 1 — a bound member needs _bind_methods + "
                    "doc/classes/*.xml + registration; Class 2 — Object/Ref/RID ownership)")
    if norm.startswith("editor/"):
        docs.append(TAXONOMY + " (Class 5 — editor code is #ifdef TOOLS_ENABLED; prove "
                    "target=template_debug still links)")
    if norm.endswith("SConstruct") or norm.endswith("SCsub") or norm.endswith("detect.py"):
        docs.append(TAXONOMY + " (Class 5 — build axes: target, dev_build, precision, "
                    "module flags and the #ifdef sets they imply)")
    return docs


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return

    tool = data.get("tool_name") or ""
    session = data.get("session_id") or "nosession"
    tool_input = data.get("tool_input") or {}

    docs = []
    if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
        path = (tool_input.get("file_path") or tool_input.get("notebook_path") or "")
        norm = path.replace("\\", "/")
        # strip a leading absolute prefix so the repo-relative rules match
        marker = "/godot-rtx/"
        if marker in norm:
            norm = norm.split(marker, 1)[1]
        docs.extend(docs_for_path(norm))
    elif tool in ("Bash", "PowerShell"):
        command = tool_input.get("command") or ""
        if re.search(r"\bscons\b", command):
            docs.append(GODOT_RULES + " (build targets, dev_build and compiledb; "
                        "never scons -c)")
        if re.search(r"--doctool\b", command):
            docs.append(TAXONOMY + " (Class 1 — regenerate doc/classes XML only after the "
                        "bind change is final, and commit the XML with it)")

    seen = set()
    docs = [d for d in docs if not (d in seen or seen.add(d))]
    if not docs:
        return

    temp = os.environ.get("TEMP") or os.environ.get("TMP") or "/tmp"
    marker_dir = os.path.join(temp, "claude", "godot-rtx-ref-hook")
    try:
        os.makedirs(marker_dir, exist_ok=True)
    except Exception:
        pass

    fresh = []
    for d in docs:
        key = re.sub(r"[\\/ :().*-]", "_", session + "-" + d)[:180]
        marker = os.path.join(marker_dir, key)
        if not os.path.exists(marker):
            try:
                open(marker, "w").close()
            except Exception:
                pass
            fresh.append(d)

    if not fresh:
        return

    msg = (
        "Reference docs relevant to this action - read them this session if you "
        "have not yet (they hold the canonical project rules): " + "; ".join(fresh)
    )
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "additionalContext": msg,
        }
    }))


main()
