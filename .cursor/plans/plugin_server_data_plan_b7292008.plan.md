---
name: Plugin Server Data Plan
overview: "Concrete work plan for the current V8 plugin (Plugin API 26): improve what we send via retail TeamSpeak (plugin commands + hub policy). No ServerLib app. No hybrid authority in v1. Inventory already in vergleichs.md."
todos:
  - id: inventory-wire
    content: "DONE — vergleichs.md inventories CEPOS, hub keys, used/unused API 26"
    status: completed
  - id: protocol-design
    content: "Write doku design: versioned CE* plugin-command map + hub schema version (doc only, freeze wire rules)"
    status: pending
  - id: hub-schema-vnext
    content: "Specify additive hub keys (feature flags, schema version) — clients read only"
    status: pending
  - id: implement-cemode
    content: "First code WP: CEMODE (or equivalent) edge send on voice-mode change — one function, keep CEPOS"
    status: pending
  - id: wire-filter-intensity
    content: "Optional follow-up WP: actually wire hubAudioFilterIntensity / FilterIntensity into PCM if still unwired"
    status: pending
  - id: whisper-experiment
    content: "Later experiment only: requestClientSetWhisperList for admin overlay — not proximity"
    status: pending
  - id: no-serverlib
    content: "Never revive ClientLib/ServerLib custom app in this workstream"
    status: pending
isProject: false
---

# Current-plugin plan: better use of retail TeamSpeak (API 26)

**Locked product:** existing V8 DLL on retail TeamSpeak (`PLUGIN_API_VERSION 26`).  
**Locked approach:** **A-only** — send/store more via TeamSpeak relay + hub description.  
**Deferred:** hybrid voice authority, radio product, ServerLib custom app.  
**Baseline inventory:** [`vergleichs.md`](../../vergleichs.md).

```text
Keep:     Pos.txt -> CEPOS -> client proximity -> PCM/3D
Improve:  cleaner peer messages + richer hub policy on TS server
Never:    expect ts3server to compute who hears whom
```

---

## 0. Constraints (from live plugin)

| Can | Cannot |
|---|---|
| `sendPluginCommand` relay (CEPOS today) | Proximity math inside retail `ts3server` |
| Hub policy in root channel description | True packet drop for inaudible speakers |
| Client PCM + 3D as hearing | Trust client position as anti-cheat truth |
| Optional later: `requestClientSetWhisperList` overlays | Replace PCM with ServerLib routing |

---

## 1. Work packages (ordered)

### WP0 — Inventory (done)

- CEPOS 56-byte layout, throttle/keepalive
- Hub `[GLOBAL]` / zones / races / defaults keys
- Used vs unused `TS3Functions`
- Documented in `vergleichs.md`

### WP1 — Protocol + hub design (doc only)

**Plugin commands (keep CEPOS wire-compatible):**

| Prefix | Purpose | When to send |
|---|---|---|
| `CEPOS:` | Position + voiceDistance (unchanged 56 B) | Throttled move + 1 Hz keepalive |
| `CEDRAIN:` | Wakeup (unchanged) | Internal |
| `CEMODE:` **(first new)** | Voice mode id + distance | On mode change only (edge) |
| `CEPING:` (later) | Seq / stale hint | Low rate |
| `CEAUTH:` (later) | Soft identity hint | On connect — still spoofable |

Rules: `sendPluginCommand` only; bounds-check; throttle; no secrets; unknown prefixes ignored.

**Hub schema (additive):** `SchemaVersion` + flags; missing keys = old defaults; clients read only.

### WP2 — First code change (Golden Rule: one function)

**Implement `CEMODE` only:**

1. On voice-mode change, send small plugin command (mode + distance).
2. Peers update without waiting for CEPOS keepalive.
3. Do not break 56-byte CEPOS.

Likely files: `ts3_cepos.c` / new cmd dispatch, `voice_modes.c`, `ts3_entry.c`, `doku/aenderungen/…`.

**Off-limits in WP2:** hub keys, whisper lists, filter wiring, radio, hybrid backend.

### WP3 — Optional: wire FilterIntensity if still unwired (ask first)

### WP4 — Optional: whisper-list admin overlay experiment (not proximity)

---

## 2. Out of scope

- ServerLib custom app / `ts3-Testing` product
- Proximity inside retail `ts3server`
- Hybrid authority (unless reopened)
- Breaking CEPOS without compatibility plan

---

## 3. Success / test

- Two clients: mode hotkey → peer sees update via `CEMODE`; CEPOS distance still works; old clients ignore `CEMODE` and keep CEPOS.

## 4. Default on “go”

Implement **CEMODE** first.
