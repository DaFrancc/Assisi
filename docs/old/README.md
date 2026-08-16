# Superseded documents — do not treat as guidance

Everything in this directory is **outdated**. It is kept for history: to show what was
considered, what was decided, and why a later plan changed course. It is not a description
of how the engine works today and must not be cited as authority.

If a document here disagrees with a current plan, with Linear, or with the code, it is
wrong. Check the code.

## Where the current material lives

| Area | Current source |
|---|---|
| Lighting and shadows | [`../lighting-and-shadows-plan.md`](../lighting-and-shadows-plan.md) — supersedes `lighting-design-notes.md` |
| Scope and ordering of all rendering work | Linear project **Rendering** (team Engine) — its description is the plan of record, milestone descriptions carry scope decisions |
| Everything else | The corresponding document at the top level of `docs/`, or Linear |

Several files here are older snapshots of documents that still exist at the top level of
`docs/` (the blueprint set, `remaining-work.md`, `replication-plan-v4.md`,
`High-level-todo.txt`). In every such case the top-level copy is the live one.

## Why these are kept rather than deleted

A rejected option with its reasoning is worth more than no record at all. When someone asks
"why aren't we using virtual shadow maps" or "why was the mobility flag dropped", the
answer should be findable rather than reconstructed. Current plans cite these documents by
name where they overturn them — but they cite them as *history*, never as instruction.
