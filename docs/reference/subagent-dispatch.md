# Subagent Dispatch

Cross-cutting rules for every delegated context. Runtime-specific call shapes,
tools, model slugs, effort, and trailers live in `.agents/adapters/`.

- Research shape: [research-mode.md](research-mode.md).
- Implementation shape: [implementation-mode.md](implementation-mode.md).

## Invariants

1. **Select the tool/model explicitly when the live runtime exposes that
   choice.** Use the active adapter's mapping and only values supported by the
   current tool. Never invent a field or model.
2. **Set effort through the runtime.** Use the adapter's approved/default
   setting without repeated permission. Prompt wording is not a substitute for
   an exposed effort parameter.
3. **Hand over navigation.** Every code-touching prompt includes the chain from
   [cpp-navigation.md](cpp-navigation.md) and requires the agent to report the
   methods used. Do not assume repository instructions or prior exploration
   automatically transfer into a fresh context.
4. **Attribute the executing agent.** When a delegated writer commits, follow
   the adapter's trailer rule and identify the model/agent that actually made
   the change. Audit trailers after multi-agent work; never fabricate a version.
5. **Use a fresh bounded context.** Provide the task-specific facts and source
   anchors needed to act. Reviewers always start fresh, including round 2; never
   resume or message the previous reviewer.
6. **Keep prompts and handoffs self-contained.** A prompt states goal,
   authority, owner-approved shape/exclusions, owned files, relevant failure
   classes, validation boundary, navigation, and `comments: default NONE` for a
   writer. A handoff states edits, evidence, validation actually performed, and
   open gaps. Do not replay the conversation or trust a summary as proof.

Multiple writers may run concurrently only on disjoint files with no dependency
on an API or behavior another writer is introducing. All contexts share the
same working tree; never use worktrees or edit a file held by another writer.
