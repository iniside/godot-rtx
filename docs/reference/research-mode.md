# Research / Search Mode

Detail for **Research / Search Mode - RULE** in
`.agents/shared/research-navigation.md`.

## Why A Text Sweep Is A Lower Bound

One `rg` pass misses overloads, virtual overrides, macros such as `GDCLASS` and
`GDVIRTUAL`, inherited members, bound script names, `#ifdef` alternatives,
`doc/classes` contracts, ProjectSettings registration, GLSL includes, `SCsub`
inputs, and serialized scene/resource names. Label text-search results as a
lower bound and state the method that produced each answer.

## When To Ask

Ask how to research only when the method can change coverage or the answer,
such as a wide API map, all-consumer sweep, render-thread/RD data-flow trace, or
upstream-versus-fork compatibility audit. For a bounded lookup, choose the
method and run it.

## Method Menu

- **LSP/clangd** for a named C++ symbol; follow
  [cpp-navigation.md](cpp-navigation.md).
- **Targeted main-agent reads** for one header/implementation, `SCsub`, XML,
  shader include graph, GDScript, or text scene/resource.
- **Git history/upstream comparison** to distinguish shipped upstream API from
  fork-local behavior and understand why a patch exists.
- **Independent read-only agents** for distinct unanswered questions across a
  broad surface. Give each question one owner, use the cheapest suitable lane,
  and synthesize the results in the main context.
- **`rg`/file listing** for bounded text/config inventory or after semantic
  navigation fails. Return to semantic/source navigation once it finds an
  anchor.

## Required Evidence For Plans

An API or behavior plan needs three kinds of evidence:

1. affected API and public/serialized contracts;
2. concrete constructors, callers, consumers, and data-flow sites;
3. behavior authority, ownership/lifecycle/threading, and relevant patterns in
   this tree.

The main agent may gather a small surface directly. Delegate only independent
questions that benefit from a separate context; do not pad to a fixed agent
count or duplicate completed research.

## Stopping Rule

Stop once evidence supports the decision or proposed change. Expand only for a
concrete dependency, contradiction, or failing case. If a point cannot be
resolved with available methods, state the uncertainty and its impact instead
of repeating the same search.
