# Altered States: design

Working name; rename freely. A Skyrim SE SKSE mod: craftable drugs, addiction, and overdose blackouts.
Built for the owner's Skyrim Together Reborn (STR) sessions. Everything below is the owner's decision unless marked **(my call)**.

## Goals
- Three base drug types, each with one drug at first: **Stimulant**, **Depressant**, **Hallucinogen**.
- Made at the **alchemy lab**, as new items from scratch (not built on Synthetic Drugs of Skyrim).
- **Addiction:** using a drug builds dependency. Withdrawal debuffs get stronger as dependency grows.
  Start with: cravings, shakes, slower stamina recovery. Tune later.
- **Overdose blackout:** taking two doses back to back (testing rule) always causes a blackout.
  The player passes out and wakes up:
  - naked, carrying only a **note** that names where their gear is;
  - somewhere random but **walkable from the gear, still far away** (my call: a few minutes on foot, not across the map);
  - with a **hangover**: lower stamina, occasional blurred vision, a headache (my call: fades over about a game day).
- The gear is moved into the **hold's own prison chest**, the one vanilla uses when you are jailed, nearest to where the player blacked out.
- The note names the prison only. **No map marker.**

## Starting drugs (my call, owner agreed)
| Type | While high | Comedown |
|---|---|---|
| Stimulant | faster movement, more stamina and stamina regen | stamina drain |
| Depressant | damage resistance, calm, slower movement | slow, drowsy |
| Hallucinogen | visual trip (colour, blur), Detect Life | disorientation |

## Non-goals (now)
- Chance-based blackouts, per-drug risk, addiction level affecting blackout chance (later).
- A random chest location for the gear each time (later idea).
- A **drug dealer** feature: selling drugs to fences only (deferred; keep drug items tagged so it is easy to add).
- Anything that needs the owner's friends to see our effects: screen effects stay local.

## Settings (MCM, short texts that fit on screen)
Back-to-back window (game minutes), enable overdose blackout, enable addiction, debug logging.

## Skyrim Together notes
- Magic effects, addiction and hangover run on the local client; other players do not see screen effects.
- The player's inventory is synced, so stripping it is visible to others.
- Chests are synced only in player homes (`bSyncPlayerHomes`, off by default), so the prison chest contents are a local change.
- Teleports are position changes the server already syncs. **Unverified:** the chest transfer and the teleport. Build and test these two first.

## Open problem to decide before the item work
The alchemy lab does not use recipes. It combines **ingredients' own effects**. So "make the drugs at the alchemy lab"
needs either (a) new custom ingredients (with our effects) placed in the world or vendors, so players mix them in the lab,
or (b) the cooking pot instead (plain recipes, simple and robust). Owner to choose.

## Build approach (my call)
Reuse what Sleep Together proved: C++23 SKSE plugin with CommonLibSSE-NG, CMake + vcpkg, MCM Helper config with a generated
plugin ESP, generated release zips, tests for pure logic, noreply commits, a private GitHub repo. Items and effects are data
(a generated ESP); addiction, blackout and relocation are plugin code.
