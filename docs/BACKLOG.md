# Highrim backlog (owner's notes for later)

Nothing open right now.

## Done

Items 1 to 6 of the owner's list were built in Task 18 (see the README, "Task 18 findings"). Everything in game is still unverified.

1. **Rimerush: no Alchemy or Enchanting boost.** Done: 16 skills on the potion and the stack abilities, "All Skills Boosted" says "All skills except Alchemy and Enchanting +N."; the withdrawal penalty stays on all 18.
2. **Condensed drug descriptions.** Done: one short sentence per effect, fewer words in effect and spell names (item names unchanged).
3. **Red negative portion on the bars.** Done as the ravage spell `HighrimRavage` (maximum reduction, as Survival Mode does it); the direct drain stays behind `Core::kDrainUsesRavage`. Whether the HUD shows the red part is unverified.
4. **Rimerush: faster attack speed.** Done: "Faster Attacks", Weapon Speed Multiplier, +20% on the potion and +5% per stack level (named constants in `tools/make_esp.py`); the value convention is unverified in game.
5. **Rimerush screen effect.** Done: none while the high lasts; the comedown look stays.
6. **Timed addiction, comedown and withdrawal.** Done: "Addicted: <drug>" and the withdrawal tiers are timed spells (length = time until the dependency is below 10, from the game's timescale), re-synced after doses and about once a minute; the comedown is a 60 s timed spell as before. Fixed-length variants are the fallback if the live duration cannot be set. Open question left for a play test: how waiting, sleeping and fast travel advance the timers.
