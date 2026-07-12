# Agent instructions — Conan Exiles TeamSpeak

**Every agent request in this repo MUST follow:**

`.cursor/rules/vibecoding-cost-efficient.mdc`

That rule has `alwaysApply: true`. Do not skip it.

## Required on every reply (line 1)

```
Vibecoding: ACTIVE · Effort: Sx · Phase: Research|Implement · Rule: vibecoding-cost-efficient.mdc
```

## Required workflow

1. Assess effort (S0–S4)
2. Research before code (S2+)
3. Delegate to subagent with matching model (see rule table)
4. Golden Rule: one function only, nothing extra
5. Rewrite from reference — never blind copy from Mumble/old plugin
6. Thread rule: TS API only on callback thread (Queue + CEDRAIN)

## Subagents

Pass this in every Task prompt:

```
MANDATORY: follow .cursor/rules/vibecoding-cost-efficient.mdc
```

Subagents may not receive project rules automatically — the parent must inject them.

## References

- `REWRITE_PLAN.md` — golden rule, thread lessons, rewrite scope
- `plan.md` — scaling phases (do not edit unless user asks)
