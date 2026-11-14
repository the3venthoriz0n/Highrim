# Highrim: design

A Skyrim SE SKSE mod: craftable drugs, addiction, and overdose blackouts.
Built for the owner's Skyrim Together Reborn (STR) sessions. Everything below is the owner's decision unless marked **(my call)**.

## Goals
- Three base drug types, each with one drug at first: **Stimulant**, **Depressant**, **Hallucinogen**.
- Made at the **cooking pot**, as new items from scratch (not built on Synthetic Drugs of Skyrim). Decided 2026-11-13: the alchemy lab mixes ingredients' own effects and has no recipes, so recipes (COBJ with the CraftingCookpot keyword) are used instead.
- **Addiction:** using a drug builds dependency. Withdrawal debuffs get stronger as dependency grows.
  Start with: cravings, shakes, slower stamina recovery. Tune later.
  Task 14 (owner's rules): every drug says it is **highly addictive** (a no-op "Highly Addictive" effect on each potion). **While high** Speech and Sneak are 0 ("Shaky and Suspicious"). **In withdrawal** all 18 skills get a flat debuff, -10 / -20 / -30 for tiers 1 / 2 / 3 (this replaces any separate Speech or Sneak penalty); the short comedown after each high does not touch skills. Addiction is visible per drug: while a drug's dependency is 10 or more the player has an "Addicted: <drug>" ability; HUD messages name the drug causing the worst withdrawal tier, say "The cravings ease." when a dose of that drug brings the tier down and "You are no longer addicted to <drug>." when dependency fades below 10. Relief is per drug: only a dose of the drug the withdrawal comes from resets that drug's 4 hour grace.
  Velvet Haze (Task 14): no armour; rapid health regeneration (+500%), carry weight +150 and invisibility for the 120 s high, plus the slowed movement; its stack abilities add more health regeneration (+100% per level) and carry weight (+50 per level).
- **Overdose blackout:** each drug has its own dose stack (a dose within the window of the previous dose of the same drug adds one; later restarts at 1) that drives the effects, and a combined counter counts doses of any drug inside the window (default 120 game minutes). When that total reaches the blackout dose count (default 4, setting `iBlackoutDoses`, 2 to 8) the player is knocked down and blacks out about 2.5 s later. Addiction: a dose adds 30, levels decay 5 per game day, withdrawal starts 4 game hours after the last dose at level 10 (tier 1), 35 (tier 2) and 60 (tier 3).
  The player passes out and wakes up:
  - naked, carrying only a **note** that names where their gear is;
  - somewhere random at a **map marker far from the gear** (my call: 15000 to 80000 game units from the prison, which should be a few minutes on foot at most; if no marker is in that band the band widens in two steps, 8000 to 150000 and then anything from 3000 up, so the walk back is not always short);
  - with a **hangover**: lower stamina, occasional blurred vision, a headache (my call: fades over about a game day).
- The gear is moved into the **hold's own prison chest**, the one vanilla uses when you are jailed, nearest to where the player blacked out.
- The note is **vague on purpose**, as if the player had a bad night and the guards found them: it names the hold's capital city only ("Last night is a blur. Shouting. Guards. A cold floor, maybe a cell. Your things might be somewhere in <City>... if the guards kept them."), never "prison" or the exact building, and no map marker. The wake-up message and the "Lost Gear: <City>?" ability use the same vague wording.

## Starting drugs (my call, owner agreed)
| Type | While high | Comedown |
|---|---|---|
| Stimulant (Rimerush, revamped in Task 15) | +10 Health, Magicka, Stamina and +10 to all 18 skills for 120 s; the most addictive (dose 40, decay 3 per day) | stamina drain, and a withdrawal that drains Health, Magicka and Stamina until a dose; a depleted bar can trigger the blackout if `bFullBlackout` allows it |
| Depressant | damage resistance, calm, slower movement, random ragdoll knockdowns (`bRagdoll`, shorter intervals with the stack, fires regardless of combat, mount, swimming etc.) | slow, drowsy |
| Hallucinogen | visual trip (colour, blur), Detect Life | disorientation |

## Rimerush withdrawal drain and the full blackout gate (Task 15)
Rimerush withdrawal (level >= 10, 4+ game hours since the last Rimerush dose) is plugin-driven: about once a real second Health, Magicka and Stamina are damaged by `0.96 * (0.5 + level/50) * min(3, 1 + hours/12)` per bar per second (Health floor 5, never killing) until another Rimerush dose. A depleted bar starts the standard blackout ("rimerush withdrawal") only when `bFullBlackout` (default OFF) and `bRimerushWithdrawalBlackout` allow it; afterwards bars go to 25% and the drain pauses 5 real minutes. `bFullBlackout` is the third, independent gate (`Blackout::FullBlackoutAllowed()`) beside `bOverdose` and `bRimerushWithdrawalBlackout`: while it is off nothing starts the full blackout (overdose, debug key, Rimerush withdrawal), the drain just rests at the floors, and no restore or pause rules run. A milder faint is a separate later task.

## Non-goals (now)
- Chance-based blackouts, per-drug risk, addiction level affecting blackout chance (later).
- A random chest location for the gear each time (later idea).
- A **drug dealer** feature: selling drugs to fences only (deferred; keep drug items tagged so it is easy to add).
- Anything that needs the owner's friends to see our effects: screen effects stay local.

## Settings (MCM, short texts that fit on screen)
Back-to-back window (game minutes), doses to blackout, enable overdose blackout, enable addiction, debug logging.

## Skyrim Together notes
- Magic effects, addiction and hangover run on the local client; other players do not see screen effects.
- The player's inventory is synced, so stripping it is visible to others.
- Chests are synced only in player homes (`bSyncPlayerHomes`, off by default), so the prison chest contents are a local change.
- Teleports are position changes the server already syncs. **Unverified:** the chest transfer and the teleport. Build and test these two first.

## Crafting (decided)
Cooking-pot recipes from vanilla ingredients (COBJ, bench keyword CraftingCookpot, no conditions). Task 13 (owner decision): every drug's recipe costs exactly 1000 gold (Gold001, count 1000) and nothing else, one potion per craft. Dreamcap's reveal life is cast by the plugin (vanilla Detect Life), not by a potion effect.

## Build approach (my call)
Reuse what Sleep Together proved: C++23 SKSE plugin with CommonLibSSE-NG, CMake + vcpkg, MCM Helper config with a generated
plugin ESP, generated release zips, tests for pure logic, noreply commits, a private GitHub repo. Items and effects are data
(a generated ESP); addiction, blackout and relocation are plugin code.
