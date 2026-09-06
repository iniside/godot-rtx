# Commit Message Format

Detail for **Commit Message Format - RULE** in
`.agents/shared/core-rules.md`.

Use the tree's established format:

```text
<area>: <Imperative subject>
```

- `area` is the touched subsystem, such as `raytracing`, `dlss`,
  `rendering_device`, `d3d12`, `scene`, `editor`, `docs`, or `arcgame`.
- Separate multiple areas with commas only when the commit genuinely spans
  them.
- Fork-wide feature commits may use the established `NVIDIA: <subject>` form.
- Keep the subject imperative, at most 72 characters, with no trailing period.
- Use the body to explain why the change is needed and any material validation
  boundary; do not narrate every edited line.

Any `Co-Authored-By` trailer identifies the agent/model that actually performed
the work. Runtime adapters define the concrete identity. Never copy a trailer
from another runtime or fabricate a model version.
