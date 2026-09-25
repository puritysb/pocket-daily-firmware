# Pocket Daily firmware: agent skills

Task procedures for every coding agent. Claude Code auto-discovers them here
and loads one when the task matches its `description`. Other agents (Codex,
OpenCode, the GitHub coding agent) find them through the skill table in the
root `AGENTS.md` and read the matching `SKILL.md` directly. They encode how
this project wants C/C++ written: the judgment calls and self-review gates that
keep the firmware small, stable, and reviewable.

These are written for capable agents, not beginners. They are principle- and
decision-focused on purpose. They deliberately avoid line-number citations,
which drift; they anchor on durable names (APIs, types, macros, files).

The root `AGENTS.md` is the always-loaded rule set shared by every agent:
constraints, gotchas, prohibitions, and conventions that apply everywhere.
`docs/embedded-reference.md` holds the mechanisms and examples behind those
rules. These skills load on demand.

| Skill | Loads when you are... |
|---|---|
| `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
| `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
| `hal-and-abstractions` | touching storage, input, display, settings, i18n, rendering |
| `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
| `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |
| `debug-crashes` | chasing a panic, reboot, hang, or watchdog timeout |
| `firmware-deploy` | getting a build onto the device, over USB or SD card |
| `fork-sync` | pulling upstream, opening an upstream PR |
| `generated-content` | changing i18n YAML, HTML pages, or fonts — anything a build script emits |

The first five are **decision procedures**: judgment `AGENTS.md` cannot afford
to carry. The last four are **workflows** kept out of `AGENTS.md` so they load
only when relevant. Each ends with a self-review checklist the agent runs
against its own diff before handing it back — those double as a fast PR rubric.

Some guidance lives in **directory-scoped** `AGENTS.md` files instead:
`lib/Epub/AGENTS.md` (cache formats and invalidation). Claude Code loads
them when it opens a file in that subtree; other agents are told by the root
`AGENTS.md` to read them before editing there.

## Maintaining these

Edit the `SKILL.md` under each directory. Keep them tight and tool-neutral: no
syntax or behavior that only one agent understands. Do not restate `AGENTS.md`;
add the judgment it cannot afford to carry. Trigger quality lives in the
`description` field: it must name the situations that should pull the skill
in, in the words a contributor's task would use. When you add, rename, or
remove a skill, update the table in the root `AGENTS.md` too.

Before adding to `AGENTS.md`, ask whether the content is needed on every task;
if it is a procedure for one task or one directory, it belongs in a skill or a
scoped `AGENTS.md`, and background belongs in `docs/embedded-reference.md`.
Never add a `CLAUDE.md` or `CLAUDE.local.md`: Claude Code would read it instead
of `AGENTS.md`.
