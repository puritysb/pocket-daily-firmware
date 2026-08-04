# CrossPoint Reader: Claude Code skills

Project skills for Claude Code. Claude auto-discovers them and loads one when the
task matches its `description`; you do not invoke them by hand. They encode how
this project wants C/C++ written: the judgment calls and self-review gates that
keep the firmware small, stable, and reviewable.

These are written for capable agents, not beginners. They are principle- and
decision-focused on purpose. They deliberately avoid line-number citations,
which drift; they anchor on durable names (APIs, types, macros, files).

`CLAUDE.md` is a **symlink** to `.skills/SKILL.md` — one file, two names, so the
Claude Code session and the GitHub coding agent read exactly the same rules and
cannot drift. It stays the always-loaded rule set: constraints, gotchas,
prohibitions, and conventions that apply everywhere. These skills load on demand.

| Skill | Loads when you are... |
|---|---|
| `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
| `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
| `hal-and-abstractions` | touching storage, input, display, settings, i18n, rendering |
| `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
| `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |
| `debug-crashes` | chasing a panic, reboot, hang, or watchdog timeout |
| `firmware-deploy` | getting a build onto the device, over USB or SD card |
| `fork-sync` | pulling upstream, opening an upstream PR, re-porting the AgentDeck contract |
| `generated-content` | changing i18n YAML, HTML pages, or fonts — anything a build script emits |

The first five are **decision procedures**: judgment CLAUDE.md cannot afford to
carry. The last four are **workflows** moved out of CLAUDE.md so they load only
when relevant. Each ends with a self-review checklist Claude runs against its own
diff before handing it back — those double as a fast PR rubric.

Some guidance lives in **directory-scoped** `CLAUDE.md` files instead, loading
only when Claude touches that subtree: `lib/Epub/CLAUDE.md` (cache formats and
invalidation) and `src/agentdeck/CLAUDE.md` (the attention-steering invariant).

## Maintaining these

Edit the `SKILL.md` under each directory. Keep them tight. Do not restate
CLAUDE.md; add the judgment CLAUDE.md cannot afford to carry. Trigger quality
lives in the `description` field: it must name the situations that should pull
the skill in, in the words a contributor's task would use.

Editing the rule set itself means editing `.skills/SKILL.md` — writing through
the `CLAUDE.md` symlink works, but tools that refuse to follow symlinks will
reject it. Before adding to it, ask whether the content is always-needed; if it
is a procedure for one task or one directory, it belongs in a skill or a scoped
`CLAUDE.md`.
