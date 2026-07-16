# Agent Instructions — Conan Exiles TeamSpeak

## Rule Sources (auto-injected — no manual read needed)

Cursor automatically loads these before every turn. Do **not** spend a tool
call re-reading them — that's the opposite of cost-efficient:

1. `.cursor/rules/vibecoding-cost-efficient.mdc` — `alwaysApply: true`
2. `~/.cursor/rules/read-project-rules-first.mdc` — user-global, always applies
3. This file (`AGENTS.md`) — always applied by Cursor, no frontmatter needed

**If `vibecoding-cost-efficient.mdc` is genuinely not visible in your context**
(check before assuming), say so in one line instead of proceeding as if
you'd read it.

## Required on every reply (line 1)

```
Vibecoding: ACTIVE · Model: {model} · Effort: Sx · Phase: {phase} · Rules: {rules} · Task: {task}
```

| Field | Values |
|-------|--------|
| Vibecoding | `ACTIVE` (default) or `INACTIVE` if user disabled cost-efficient mode |
| Model | Current agent model for this turn (e.g. `Composer`, `GPT-5.6`) |
| Effort | `S0`–`S4` — see effort scale in `vibecoding-cost-efficient.mdc §Assessment` |
| Phase | `Research` \| `Implement` \| `Review` \| `Plan` |
| Rules | Primary rule filename; append `+name` for each extra rule loaded this turn |
| Task | ≤8 words: what is actively being worked on |

Example:

```
Vibecoding: ACTIVE · Model: Composer · Effort: S2 · Phase: Implement · Rules: vibecoding-cost-efficient.mdc · Task: Expand agent status header
```

*(If the S0–S4 scale or subagent/model table isn't actually defined in
`vibecoding-cost-efficient.mdc` yet, add it there — this header can't be
filled in reliably against an undefined scale.)*

## Required workflow

1. Assess effort (S0–S4) before starting.
2. Research before code — required at S2 and above.
3. Delegate to a subagent with the matching model (see rule table).
4. Golden Rule: one function per change, nothing extra.
5. Rewrite from reference — never blind-copy from Mumble/old plugin.
6. Thread rule: TeamSpeak API calls only on the callback thread (Queue + CEDRAIN).

## Subagents

Subagents don't automatically receive project rules — inject this in every
`Task` prompt:

```
MANDATORY: follow .cursor/rules/vibecoding-cost-efficient.mdc
```

## References

- `REWRITE_PLAN.md` — golden rule, thread lessons, rewrite scope
- `plan.md` — scaling phases (do not edit unless the user asks)
