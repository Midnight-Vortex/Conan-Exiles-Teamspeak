# Agent instructions — Conan Exiles TeamSpeak

**Every agent request in this repo MUST follow ALL rules in `.cursor/rules/` with `alwaysApply: true`**, especially:

- `.cursor/rules/vibecoding-cost-efficient.mdc` — **this repo's** workflow (effort tiers, subagents, thread contract, build)
- All other `.cursor/rules/**/*.mdc` — **general** engineering principles (architecture, security, testing, docs, etc.)

Do not skip any alwaysApply rule.

## Before any tool call (mandatory)

1. Read **this file** (`AGENTS.md`).
2. Read **`.cursor/rules/vibecoding-cost-efficient.mdc`** (full file if not already in context).
3. User-global: `~/.cursor/rules/read-project-rules-first.mdc` also applies.

## Required on every reply (line 1)

```
Vibecoding: ACTIVE · Effort: Sx · Phase: Research|Implement · Rule: vibecoding-cost-efficient.mdc
```

## This repo only (vibecoding)

1. Assess effort (S0–S4)
2. Research before code (S2+)
3. Delegate to subagent with matching model
4. Golden Rule: one function only, nothing extra
5. Rewrite from reference — never blind copy from Mumble/old plugin
6. Thread rule: TS API only on callback thread (Queue + CEDRAIN)

Subagents: pass `MANDATORY: follow .cursor/rules/vibecoding-cost-efficient.mdc` in every Task prompt.

## Documentation duty (V8+, mandatory)

Every code change MUST be documented in `doku/` per
`.cursor/rules/05-documentation/doku-pflicht.mdc`: what changed, why it is
better/more stable than before, and how it works (beginner-friendly, German).
A work package without its doku entry is **not done**.

## References

- `REWRITE_PLAN.md` — V7 golden rule, thread lessons, rewrite scope (historical)
- `REWRITE_PLAN_V8.md` — current V8 rewrite plan (goals, phases, agent routing)
- `doku/` — beginner-friendly documentation (architecture, modules, change log)
- `plan.md` — scaling phases (do not edit unless user asks)
