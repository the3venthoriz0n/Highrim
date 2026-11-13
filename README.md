# Highrim

A Skyrim SE SKSE mod (version 0.1.0): three craftable cooking-pot drugs, per-drug addiction with escalating withdrawal, and an overdose blackout. Keep taking the same drug and its effects stack, until you black out: your gear is moved to the nearest hold's prison chest and you wake up naked, with a note, at a distant map marker, six hours later, with a hangover.

**Status: v0.1.0, built and unit-tested, but never run in Skyrim.** See "Verified vs unverified" below before trusting any behaviour.

## Requirements

- Skyrim Special Edition (the game version CommonLibSSE-NG v15.7.3 targets) with SKSE64.
- Address Library for SKSE Plugins (CommonLibSSE-NG resolves addresses through it).
- SkyUI and MCM Helper for the settings menu (the mod works with the defaults without it).
- `Highrim.esp` is a light (ESL) plugin and needs only Skyrim.esm.

## Install

Install `Highrim-0.1.0.zip` with a mod manager (it contains `Data/SKSE/Plugins/Highrim.dll`, `Data/Highrim.esp` and the MCM files under `Data/MCM`), enable `Highrim.esp`, and start the game. The log is `Documents\My Games\Skyrim Special Edition\SKSE\Highrim.log` and starts with `Highrim 0.1.0 loading`.

## The drugs

Craft them at a cooking pot.

| Drug | Recipe | Effect for 120 s | Comedown (60 s) |
|---|---|---|---|
| Rimerush (stimulant) | 2 Giant Lichen + 1 Glowing Mushroom | Movement speed +34%, stamina +68, stamina regeneration +135% | Stamina regenerates 60% slower |
| Velvet Haze (depressant) | 2 Lavender + 1 Hanging Moss | Armor rating +200, movement slowed 27% | Stamina regenerates 40% slower |
| Dreamcap (hallucinogen) | 2 Glowing Mushroom + 1 Swamp Fungal Pod | Drugged visuals (the vanilla sleeping-tree sap effect) plus the four Detect Life effects of the vanilla Detect Life scroll (areas 200 and 100 ft, for enemies and for friends) | Drugged visuals again |

Magnitudes are about 35% above the first version (Rimerush was 25 / 50 / 100, Velvet Haze 150 / 20); they live in `tools/make_esp.py`.

The comedown is cast about 120 real seconds after a dose. Drinking another dose of the same drug before then restarts that timer, so only one comedown is cast.

## Velvet Haze ragdolls (bRagdoll)

- While a Velvet Haze high lasts (from the dose until its comedown, about 120 real seconds after the last dose) the plugin knocks the player over at random intervals in real seconds, shorter with the depressant stack: stack 1 every 30 to 80 s, stack 2 20 to 55 s, stack 3 12 to 35 s, stack 4 and up 8 to 22 s. Never within 8 s of a dose, and never twice within 6 s.
- Guards: a knockdown only happens when the player is loaded and alive, not in bleedout, combat, mounted, swimming, sitting, sleeping, in furniture, flying, transformed, in a killmove, in midair or in a scene, no menu is open and the game is not paused or in dialogue, movement controls are enabled, no blackout is running, and the player is not already ragdolled or staggered. When a guard blocks, the knockdown is postponed (the interval is not used up) and happens once the guards clear; each postponement reason is logged once.
- `bRagdoll` off means never. The schedule is cleared on load, new game and when a blackout starts.
- Route: `Actor::PushActorAway` does not exist in CommonLibSSE-NG, so the plugin calls `AIProcess::KnockExplosion` from a point just behind the player with a small magnitude (`kKnockMagnitude` in `src/Ragdoll.cpp`), and falls back to the `ragdollInstant` animation event. The log line is `Ragdoll: knocked down (stack N, next in S s, route R)`. UNVERIFIED in game: that either route ragdolls the player at all, and that the force is a stumble.

## Addiction and withdrawal (bAddiction)

- Each drug has its own addiction level, 0 to 100. A dose adds 20 (capped at 100) after first decaying the level.
- Levels decay by 10 per game day (floor 0).
- Withdrawal tier, per drug, from its level and the time since its last dose: tier 0 if the level is under 10 or the last dose was under 6 game hours ago; otherwise tier 1 under 40, tier 2 under 70, tier 3 at 70 or more. The highest tier over the three drugs applies, and only one withdrawal ability is on the player at a time.
- Tier 1 "Cravings": stamina regenerates 20% slower.
- Tier 2 "Shakes": stamina regenerates 35% slower, stamina -25.
- Tier 3 "Sickness": stamina regenerates 50% slower, stamina -40, health regenerates 50% slower.
- At withdrawal tier 2 or higher (needs bAddiction), and while the hangover is active (also with bAddiction off), an 8 second "Blurred Vision" pulse is cast every 60 to 180 real seconds. It is never cast in menus, dialogue, the console or during a blackout (a due pulse fires once the menu closes).
- Levels are kept in the SKSE co-save. After loading, the withdrawal ability is re-applied once, never stacked.

## Stacking doses and screen effects

Each drug has its own stack. A dose of a drug inside `iDoseWindowMinutes` game minutes (default 30) of the previous dose of the same drug adds one to that drug's stack; a dose after the window restarts it at 1. Doses of other drugs do not touch it. Stacks are kept in memory only (they reset on load, new game and blackout).

- **Stronger effects.** From stack 2 the plugin adds an ability for that drug and level (stack 2, 3, 4 and up = level 1, 2, 3) on top of the potion's own effects. Only one stack ability per drug is on the player; the previous level is removed first. All of a drug's stack abilities are removed when its high ends (the comedown moment, about 120 real seconds after its last dose), on load, new game and when a blackout starts, and none are added once a blackout is on its way.

| Drug | Level 1 / 2 / 3 extra effects (strictly stronger each level) |
|---|---|
| Rimerush Surge I / II / III | Movement speed +10 / 20 / 30%, stamina +25 / 50 / 75, stamina regeneration +50 / 100 / 150% |
| Velvet Haze Weight I / II / III | Armor rating +50 / 100 / 150, movement slowed 8 / 16 / 24% |
| Dreamcap Daze I / II / III | Movement slowed 5 / 10 / 15%, stamina regenerates 15 / 30 / 45% slower (Dreamcap has no stat effect of its own, so its stack is a clumsy trip; my call) |

- **Screen effects.** On every dose, and again about every 10 real seconds while the high lasts, the plugin triggers a vanilla image space modifier for the drug type, at strength 0.6 at stack 1 rising in equal steps to 1.0 at stack 4 and above. Picked by editor ID from `Skyrim.esm`, not from looking at them:
  - Stimulant: `QuickReflexesImod` (0010C761), an adrenaline look. Other candidates: `BerserkerMainImod` (red, probably too aggressive), `NightEyeImod`, `MAGShoutSprint03`.
  - Depressant: `ParalyzeMassImod` (00031060), heavy blur and drained colour. Other candidates: `defaultDesaturateImod`, `PoisonHitImod`, `CGDragonBlurImod`.
  - Hallucinogen: `DA13PeryiteVisionImod` (000C1966), strong tinted vision. Other candidates: `DA16DreamState`, `dunSleepingTreeCampISMD` (the existing trip, judged not intense enough), `WerewolfVisionImod`.

  If a pick looks wrong in game it is one constant in `tools/make_esp.py` (the three IDs are written to `src/Forms.h`).

## Overdose blackout (bOverdose, iBlackoutDoses)

When one drug's stack reaches `iBlackoutDoses` (default 4, 2 to 8) the dose is an overdose. Mixing different drugs no longer causes a blackout. `bOverdose` switches blackouts off. About 1.5 real seconds after that dose:

1. Fade to black (1.5 s) and disable player controls.
2. Pick the hold: if you are in a hold capital's own worldspace (Windhelm, Whiterun, Solitude, Markarth, Riften, including their interiors) that hold's prison; otherwise the hold whose prison is nearest to you in Tamriel coordinates. Inside an interior the position comes from the nearest door chain out to an exterior; inside any other child worldspace (caves, ruins) it is the Tamriel position of that worldspace's entrance. The prison positions, the capital worldspaces and the entrances are not read from the running game (cells are often not loaded) but from a table generated out of `Skyrim.esm` (`src/GameData.h`, see below). Each decision is logged: where you were, every candidate with its distance, and which one was chosen and why.
3. Use that prison's confiscation chest.
4. Unequip everything, move every non-quest item (gold included) to the chest and verify the counts. A mismatch rolls back and aborts with your items untouched, plus a message box. Quest items stay on you.
5. Pick a wake spot: any map marker of Tamriel (all of them, not just the ones you have discovered; the list comes from `Skyrim.esm` plus whatever the game lists in Tamriel's persistent cell, so markers added by other mods count when readable). It is chosen at random from the first band that has candidates, measured from the prison: 15000 to 80000 units, else 8000 to 150000, else 3000 and up; markers closer than 3000 are never used. The log names the band ("stage"), the marker, its position and its distance. This is chosen before anything moves; if there is no marker in any band the blackout aborts with your items untouched. (Rough scale, an estimate: 15000 units is about 200 m and takes under a minute to run, 80000 units a few minutes; the later bands can mean longer.)
6. Teleport to that marker, give you a "Crumpled Note (Hold)" reading "Your things are in the Hold prison.", advance game time by 6 hours, and add the Hangover ability (stamina -30, stamina regenerates 25% slower) for 24 game hours.

Then it holds black for 2 s and fades back in. (The log numbers the steps as above but runs step 5 before steps 3 and 4.) It does nothing if a blackout is already running, you are in combat or dialogue, controls are not available, you are dead, or the game is paused or any menu is open. An overdose triggered while a menu is open (for example the last dose from the inventory) waits until the menus close and is dropped (`overdose dropped` in the log) after 30 real seconds of waiting. Just before it moves any item and again before the teleport, the blackout re-checks that you are alive, loaded in, and that no load, new game, revert or death happened since it started; if something changed it aborts (putting back anything already moved). A load or new game also clears a running blackout and pending overdose and comedown timers. Non-playable items (other mods' tokens, summoned bound weapons) stay on you and are logged as skipped. Starting a blackout cancels a pending comedown, clears the stacks and removes the stack abilities.

Winterhold: vanilla Winterhold has no prison chest in Skyrim.esm (it has a jail marker but no confiscation container), so it is never chosen. The other eight holds (Whiterun, Eastmarch, Rift, Haafingar, Hjaalmarch, Reach, Falkreath, Pale) are always candidates: all of them have an anchor in the generated table.

`src/GameData.h` is generated by `python -I tools/extract_game_data.py <path to Skyrim.esm>` (and checked with `--check`; `tests/test_game_data.py` does that and skips if the game is not installed). It holds each hold's prison position in Tamriel (the jail marker if it lies in Tamriel, otherwise the mean position of the Tamriel doors into that city worldspace), the Tamriel position of each child worldspace, and the 330 Tamriel map markers. Run it again if you change the list of holds.

## MCM options (section General)

| Key | Default | Meaning |
|---|---|---|
| `bEnabled` | 1 | Whole mod on or off |
| `bOverdose` | 1 | Reaching the blackout dose count causes the blackout |
| `bAddiction` | 1 | Doses build addiction and withdrawal |
| `iDoseWindowMinutes` | 30 | Game minutes between doses of the same drug that stack (slider 5 to 240, step 5) |
| `iBlackoutDoses` | 4 | Same-drug doses in the window that cause a blackout (slider 2 to 8, step 1) |
| `bRagdoll` | 1 | Velvet Haze knocks the player over at random intervals |
| `bDebug` | 0 | Extra logging (for example `Dose: equip event` lines) |
| `iDebugBlackoutKey` | 0 | DirectX scan code of a key that forces a blackout; 0 means none |

## Skyrim Together

All effects are local and nothing calls the server, so each player runs their own addiction, blackout and withdrawal. Whether the teleport, item transfer and fades behave under Skyrim Together has not been tested.

## Verified vs unverified

| Part | Status |
|---|---|
| Core logic (dose stacks, blackout dose count, stack level and screen strength, addiction, decay, withdrawal tiers, serialisation, hold choice, staged wake spot pick, timers) | Verified by unit tests (ctest) |
| `src/GameData.h` (prison anchors, worldspace entrances, Tamriel markers) | Verified equal to what the generator reads from `Skyrim.esm` (`tests/test_game_data.py`); the numbers are vanilla data, not checked against the game running |
| `Highrim.esp` structure (records re-parsed, vanilla IDs checked against Skyrim.esm) | Verified by `tests/test_esp.py` parsing only |
| Release build and the packaged zip contents | Verified by building and listing the archive |
| The plugin and ESP loading in Skyrim, the MCM menu | Unverified in game |
| Drugs craftable and usable, effect magnitudes as listed, dose detection (`TESEquipEvent`) | Unverified in game |
| Comedown and blur spells, withdrawal abilities applying and removing | Unverified in game |
| Stack abilities applying, stepping up and being removed; the raised base magnitudes | Unverified in game |
| The three screen effects (picked by editor ID only): that they look intense, trigger through `ImageSpaceModifierInstanceForm::Trigger` at the given strength and re-trigger every 10 s | Unverified in game |
| Blackout: prison and chest discovery, item move, teleport, wake spot choice, 6 h skip, fades, hangover | Unverified in game |
| Velvet Haze ragdolls: that `KnockExplosion` (or the `ragdollInstant` fallback) ragdolls the player, the force, the guards (including the scene and knock state checks) | Unverified in game |
| Ragdoll interval table and scheduler (post-dose quiet time, 6 s minimum gap, stack change, reset) | Verified by unit tests (ctest) |
| Co-save of addiction and hangover | Unverified in game |
| Skyrim Together compatibility | Unverified in game |
| The GitHub release workflow | Not run yet |

## First run checklist

Open `Highrim.log` after the first launch and look for:

1. `Highrim 0.1.0 loading` at startup.
2. Between `Prison discovery: start` and `done`, one `Prison:` line for each of the nine holds (Whiterun, Eastmarch, Rift, Haafingar, Hjaalmarch, Reach, Falkreath, Pale, Winterhold). Eight should show a chest (`chest[faction]` or `chest[fallback]`); Winterhold should show `chest[missing]`.
3. After drinking a potion, a `Dose:` line, for example `Dose: Rimerush stack 1/4 level 0 strength 0.60`. No line means dose detection does not work.
4. The same drug again inside the window: `stack 2/4 level 1 ...` and `Stack: Rimerush level 1 ability, AddSpell -> true`; at the fourth dose `blackout=true`, then `Dose: overdose, starting the blackout`, then the `Blackout: step N` lines and `finished (success)`. Any `ABORT` or `MISMATCH` line explains a refused blackout.
5. After waiting 6 game hours following a dose: `Withdrawal: tier 0 -> 1 (poll), AddSpell -> true` (and `Withdrawal: cast blur pulse` at tier 2 or higher).
6. About 120 real seconds after a dose: `Comedown: cast ... comedown`.

Please report which of these lines are missing or wrong.

## Not in 0.1.0 (deferred)

- A dealer who sells the drugs (the `HighrimDrug` keyword is already on all three for it), and fences who return gear.
- A random location for the gear instead of the nearest prison.
- A chance-based blackout (it is guaranteed at the blackout dose count for now).

## Build

    git submodule update --init --recursive
    build_release.bat      (from a VS x64 command prompt)
    make_archive.bat       (writes dist\Highrim-0.1.0.zip)

Tests: `ctest --test-dir out\build-release -C Release --output-on-failure`.

The plugin `package/Data/Highrim.esp` and `src/Forms.h` are generated by `python -I tools/make_esp.py` (records cloned from vanilla templates in `tools/vanilla_templates.json`); `python -I tests/test_esp.py` re-parses the result. The plugin structure is verified by parsing only, not by loading it in the game.

Form IDs are stable across versions: every record that shipped in 0.1.0 is pinned to its 0.1.0 local ID in `tools/pinned_form_ids.json` (never edit or remove an entry; a record dropped from the generator goes under `retired` so its ID stays reserved). New records get the next free ID after the highest pinned ID, handed out in the generator's fixed order, so output stays byte-for-byte reproducible. The test suite fails if a pinned record is missing or its ID moves.

Design: `docs/superpowers/specs/2026-11-13-highrim-design.md`.

## Test it (blackout, UNVERIFIED in game)

Nothing below has been run in Skyrim yet. Every step logs to `Documents\My Games\Skyrim Special Edition\SKSE\Highrim.log`.

1. In the MCM (Highrim) set the debug blackout key (`iDebugBlackoutKey`), or put its DirectX scan code in `Data\MCM\Settings\Highrim.ini`.
2. Load a save with a few items, gold and some worn gear, stand in or near a hold, and press the key (no menu open, not in combat).
3. Expect: fade to black, you wake naked far from the prison, a "Crumpled Note", and "You wake up with a splitting headache."

Log lines that prove each step:

- Startup: `Prison: hold=<Hold> faction=... chest[faction|fallback|missing]=... jail[...]=... tamriel anchor=(x,y)` once per hold, then `Tamriel wake markers: table N / found N / usable N / no cell N ...`, between `Prison discovery: start` and `done`. Winterhold has no vanilla confiscation chest (`chest[missing]`) and is skipped. `usable` well below `found` means many markers have no cell to teleport into.
- Step 1 fade and controls: `Blackout: step 1`, `applied FadeToBlack`, `player controls disabled`.
- Step 2 hold: `step 2: player in worldspace ..., Tamriel position (x,y); <how>`, `candidate <Hold> anchor (x,y), N units from the player`, `chose <Hold> (<reason>; chest ...)`.
- Step 5 wake spot (picked before anything moves): `step 5: N markers: table ... live ...`, `stage S (<band>), N candidate(s) in the band; wake spot marker <id> at (x,y), D units from the prison anchor`.
- Steps 3 and 4 items: `step 3: using chest`, `step 4: unequip ...`, `moved N item stack(s) to the chest, verified; kept N quest item(s)`. Any `MISMATCH` line is followed by `rolling back` and an `ABORT` line, and a message box says nothing was taken (or, if the rollback logged an item missing from the chest, that some stacks may still be in it).
- Step 6: `teleporting to marker`, `position (...) -> (...)`, `note given`, `game time advanced 6 h`, `hangover ability added`, `hangover expires at game hour`.
- Fade in: `sequence done, holding black`, then `stopped FadeToBlackHold`, `applied FadeToBlackBack`, `player controls enabled`, `finished (success)`.
- Hangover removal (24 game hours later, wait in game): `hangover expired ... ability removed -> true`.

Then walk to the chosen hold's prison and open the chest named in `step 3`; your gear should be there. Ignored requests log a reason (`already running`, `in combat`, `not controllable`, `in dialogue`, `the player is dead`).

Known unverified points: whether the crime faction exposes its chest and jail marker at runtime (discovery logs it, a Skyrim.esm id table is the fallback), the exterior-position search through doors (and whether `GetSaveParentCell` is enough when a cell is not attached), the player's `cachedWorldSpace`/`exteriorPosition` fallback, whether Tamriel's map markers can be teleported to when their cell is not attached (`MoveTo` onto a marker, the log says `cell attached` or not at teleport), whether game positions equal the table (`moved` count in the marker log), advancing game time by editing the calendar globals, and the fade image-space modifiers.

## Known limitations

- Hold choice for the capital worldspaces and wake spot bands is decided from vanilla data. A mod that moves a prison, or adds a hold, is not known to the table (an added map marker is, if the game lists it). Worldspaces outside Tamriel's family (Solstheim) use the player's raw position and are logged as such; a child worldspace without a Tamriel door (for example Dragonsreach's separate worldspace) falls back to the same.
- The wake band can be wide (up to 150000 units, or even farther) in the later stages, so the walk back to the prison is not guaranteed to be short.
- Skyrim Together's time sync may undo the 6 hour skip (the hangover expiry was computed after the skip, so it would then last longer in real game time).
- The in-game date can lag a day behind after the skip, because only the hour and days passed are changed, not the day and month.
- A missing or corrupt co-save loses the hangover expiry. On load, if you still carry the Hangover ability and no expiry is recorded, the expiry is set to now plus 24 game hours.
- Changing `bDebug` in the MCM needs a game restart to change the log level.
- Controls are always re-enabled after a blackout, even if another mod had disabled them before.
- Worn gear stays unequipped (in the inventory) if a blackout aborts after the unequip step.
- Moving worn or enchanted items to the chest and back keeps the stack but, on rollback with duplicate base items in the chest, may return an older instance of the same item.
- A mounted player or a player in a transformed form is not specially handled.
- The comedown timer runs in real time, so it keeps counting while menus are open.

## Test it (doses, overdose, addiction, UNVERIFIED in game)

Nothing below has been run in Skyrim yet. Drink the potions from the inventory (not `additem` alone: only a real equip/consume counts). Set `bDebug` for extra `Dose: equip event` lines.

- One dose: `Dose: Rimerush stack 1/4 level 0 strength 0.60`, then `Dose: Rimerush addiction 20.0/0.0/0.0 blackout=false (game hour ...)`. About 120 real seconds later: `Comedown: cast Rimerush comedown`. Drinking another dose before then restarts that timer (only one cast).
- The same drug again inside the window (default 30 game minutes): `stack 2/4 level 1 strength 0.73`, the `Stack: ... AddSpell` line (the previous level is removed first), and with `bDebug` a `Visual:` line about every 10 s while the high lasts. Other drugs keep their own stacks. At the dose that reaches `iBlackoutDoses` the log says `blackout=true`, then about 1.5 s later `Dose: overdose, starting the blackout` and the `Blackout:` lines above.
- About 120 s after the last dose of a drug its `Stack: removed ... ability` lines appear. A save load or new game removes all stack abilities and resets the stacks.
- With `bAddiction` off the levels stay `0.0` but stacking and blackout still work; with `bOverdose` off `blackout=false` is always logged and no blackout starts.
- Save and reload: `State: saved (N bytes), hangover expiry ...` on save, `State: loaded, levels ...` on load. A damaged record logs `State: corrupt record, reset to zero`. After a blackout, reload within 24 game hours: `State: hangover restored ...` (or `already on the player`).
- Loading a save or starting a new game cancels pending comedowns.

Unverified: that `TESEquipEvent` fires with `equipped=true` for the player when a potion is drunk (and not for other equips of these forms), that the instant-cast comedown spell applies, and the co-save round trip inside the game.

## Test it (withdrawal, UNVERIFIED in game)

Nothing below has been run in Skyrim yet. Needs `bAddiction` on.

- Take a dose, wait 6 game hours (for example with the Wait menu): within about 5 real seconds the log shows `Withdrawal: tier 0 -> 1 (poll), AddSpell -> true` and the effect is in the active-effects list. Higher levels or longer waits move it up; each change logs `tier A -> B` and the old ability is removed first (`removed tier N ability`), so only one is ever present.
- Save and reload: `tier -1 -> N (poll)` once after the load (the co-save keeps addiction, not spell state), never twice.
- Switch `bAddiction` (or `bEnabled`) off: `addiction/mod off, removing abilities` and the effect disappears.
- Blur pulse: at tier 2 or higher, or while the hangover is active, `Withdrawal: cast blur pulse` every 60 to 180 real seconds, never in menus, dialogue, the console or during a blackout (a pulse that comes due then fires once the menu closes).

Unverified: that `AddSpell`/`RemoveSpell` on the player apply and remove the ability effects immediately, that a spell cast with `CastSpellImmediate` from the instant caster gives the blur effect, the 8 s duration in game, and the menu gating (`GameIsPaused` plus the dialogue, loading, main menu and console checks).

## License

CommonLibSSE-NG is GPL-3.0-or-later and is linked into the plugin, so Highrim is GPL too (see COPYING).

## Releasing

To release, tag `main`: `git tag v0.1.1 && git push origin v0.1.1`. The tag is the version: the workflow `.github/workflows/release.yml`
stamps it into `CMakeLists.txt`, `vcpkg.json`, `src/main.cpp` and `tools/make_archive.ps1` (it stops if any of them does not match
exactly once), builds on Windows, packages the zip, creates a GitHub release, uploads the zip to Nexus Mods as a new version of the
existing file, and finally commits the bumped version numbers back to `main` (if `main` moved in the meantime, or is protected,
that last step only warns). The tag must look like `v1.2.3` and its commit must be on `main`.

One-time setup (done): secret `NEXUSMODS_API_KEY` and variables `NEXUS_FILE_ID` and `NEXUS_MOD_ID`; without them the Nexus step is
skipped. The workflow has not been run yet, so the first run may need fixes.
