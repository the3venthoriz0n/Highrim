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
- **Overdose blackout:** each drug has its own dose stack (a dose within the window of the previous dose of the same drug adds one; later restarts at 1) that drives the effects, and a combined counter counts doses of any drug inside the window (default 120 game minutes). Task 16 changed the rule from a guarantee to a roll: from the threshold (default 4, setting `iBlackoutDoses`, 2 to 8) up, every counted dose rolls a FULL blackout with chance min(60%, 15% x (total - threshold + 1)); if that fails a **faint** happens half the time, otherwise nothing. The player is knocked down first either way (about 2.5 s). A faint is the common outcome: fade to black for about 6 s, one game hour passes, no item or position change, a 4 game hour hangover (a longer one is kept), HUD "You pass out for a moment..." / "You come to, head pounding.". The full blackout (everything below) is rare and OFF by default (`bFullBlackout`): with it off a rolled full blackout becomes a faint, so faints happen regardless. While the total is inside the window the player carries one "Overdose Risk" ability (Low = total 1 to threshold-2, Elevated = threshold-1, High = threshold or more; Low is skipped when the threshold is 2) and, at Elevated and High, a pulsing darkening screen effect (strength 0.4 / 0.8). `bOverdose` switches the whole system off. Rimerush's withdrawal drain blackout stays deterministic. Addiction: a dose adds 30, levels decay 5 per game day, withdrawal starts 4 game hours after the last dose at level 10 (tier 1), 35 (tier 2) and 60 (tier 3).
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

## Dense effect lists (Task 17)
A potion or ability that lists many effects is unreadable in a tooltip (Rimerush listed 22). The 54 per-skill magic effects (Rimerush potion, Rimerush stack abilities, withdrawal tiers) carry the Hide in UI flag (bit 15, `0x00008000`, checked against Skyrim.esm; not 0x00040000) and stay applied. One visible descriptor effect per group shows the buff or penalty: "All Skills Boosted" (potion and stack abilities) and "All Skills Reduced" (withdrawal), each a Fortify-style clone on Variable10 (actor value 77, unused by vanilla magic effects) whose magnitude is the tier value, so it has no gameplay effect of its own. Rimerush's potion shows 5 lines (Health, Magicka, Stamina, All Skills Boosted, Highly Addictive). The diagnostics count visible and hidden effects together (Rimerush 23). Whether the game really hides flagged effects is unverified.

## Task 18: the owner's backlog
- Rimerush buffs 16 skills (not Alchemy, not Enchanting; withdrawal still lowers all 18), adds "Faster Attacks" (Weapon Speed Multiplier, +20%, +5% per stack level) and has no screen effect while high.
- Every drug and effect text is one short sentence.
- The Rimerush withdrawal drain lowers the maximums with a Survival Mode style ravage spell (`HighrimRavage`, Peak Value Modifier, magnitude written at runtime); the direct drain stays as a fallback constant. Unverified: that the loss shows as the red part of the bars.
- "Addicted: <drug>" and the withdrawal tiers are timed spells whose length is the time until the dependency is below 10 (real seconds from the game's timescale), set on the live effects after the cast and re-synced after doses and about once a minute, with fixed-length variants as a fallback. The comedown stays a 60 s timed spell. Details and findings are in the README ("Task 18 findings").
