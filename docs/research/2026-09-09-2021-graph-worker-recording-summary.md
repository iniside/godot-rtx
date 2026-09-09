# Internal graph recording boundary

Checked 2026-09-09 UTC at `999bbd18cc`, with unchanged graph/Vulkan source.
This is a bounded Step 6 handoff under the approved rendering performance repair
plan, not implementation or runtime evidence. Step 5 remains with its author.

Question: which existing recording state prevents dispatching graph commands
concurrently, and can the dormant secondary helper cover compute/RT?

`RenderingDeviceGraph::_run_draw_list_command():936` decodes immutable
instruction bytes with a local cursor. Compute (`:852`) and RT (`:811`) have
the same broad replay boundary. The draw helper's optional PRINT_DRAW_LIST_STATS
counter is global mutable state. Vulkan descriptor-bind scratch at
`RenderingDeviceDriverVulkan::command_bind_render_uniform_sets():5942` is
thread-local; viewport/attachment scratch uses local allocations. This bounded
inspection is not a complete driver thread-safety proof.

The existing `_run_secondary_command_buffer_task():1082` only replays a draw
list and supplies subpass zero. `command_buffer_begin_secondary():3646` in the
Vulkan driver unconditionally dereferences render-pass/framebuffer IDs and
uses RENDER_PASS_CONTINUE. It therefore cannot serve compute/RT recording
unchanged. A Step 6 implementation needs the actual non-render recording
contract and matching sibling-driver closure, not null arguments to this helper.

Graph recording still owns one `command_data` arena and one draw/compute/RT
instruction list (`rendering_device_graph.h:845`). Resource-usage publication
also writes ResourceTracker list indices (`add_draw_list_usage():2446`).
Concurrent frontend instruction production must use isolated contexts and merge
usages through the existing graph authority. Merely activating driver replay
leaves this frontend work serial. Public RD ownership guards remain intact.

`add_draw_list_end():2480` stores a primary command and copied instructions.
`end():2739` computes graph levels/order, groups barriers and replays all commands.
Its sort scratch is thread-local, but barrier groups, resource history and driver
workaround state belong to the graph. A single graph compile owner can run on
a worker; independent recording jobs consume its frozen results. GPU dependency
order is preserved at execution even when independent CPU recording overlaps.

Before draw recording, `_get_draw_list_render_pass_and_framebuffer():900`
resolves final attachment load/store operations and may insert a framebuffer
cache entry/create driver resources. That mutable cache stage must finish under
one owner before concurrent readers. `_run_render_commands():1098` also owns
primary command-buffer splits, labels and driver callbacks. Their documented
thread/ordering constraints must survive task dispatch.

Vulkan command pools retain their created-buffer vector, and buffer creation
uses `resources_allocator` (`command_buffer_create():3609`). Provision pools and
contexts under their owning contract; command-pool reset/free must not overlap
recording. CPU job completion and GPU resource retirement remain separate gates.
No claim is made here that the allocator supports concurrent provisioning.

Navigation: clang-nav/clangd against root compile_commands, actual method/header
reads and scoped Git history. One-shot helper references returned only the
method itself, so they are not used as an exhaustive caller inventory. This
note reuses earlier dormant-helper findings and closes only the state and
non-render secondary-contract questions. No source changes, builds, automated
tests or runtime launches accompany this research.
