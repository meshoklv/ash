# Design notes

The notes below are the author's working documents, written in **Russian**. They describe how each module was built and checked; markers used throughout: **[проверено]** verified by a command or code reading, **[оценка]** estimate before a measurement, **[версия]** hypothesis, **[придумал]** own design decision with no external source. Where a note disagrees with a hardware measurement, the measurement wins.

| File | About |
|---|---|
| [`build.md`](build.md) | toolchain, building, PPSSPPHeadless recipe, running on a PSP |
| [`pipeline.md`](pipeline.md) | asset pipeline: your game copy -> PSP data, what each `tools/` script does |
| [`hw-pitfalls.md`](hw-pitfalls.md) | hardware vs emulator: bugs found on the first real-PSP run, how `src/diag` catches crashes |
| [`hw/`](hw/) | raw logs from a real PSP Street E1000 (bench, 64 MB memory test, 26-minute play session) |
| [`psp-cost-model.md`](psp-cost-model.md) | PSP-est: estimating frame cost from an emulator run |
| [`render.md`](render.md) | the optional look layer (fog, light, effects) and fast zone drawing |
| [`world.md`](world.md) | zone packages and world rendering |
| [`collision.md`](collision.md) | collision format, player capsule, camera sweep |
| [`combat.md`](combat.md) | combat module: hitboxes, damage, stamina, poise, parry, backstab |
| [`ai-runtime.md`](ai-runtime.md) | running the original enemy AI bytecode on Lua 5.0.3 + C host |
| [`boss.md`](boss.md) | the Asylum Demon runtime |
| [`events.md`](events.md) | map event scripts: doors, fog gates, loot |
| [`items.md`](items.md) | items, inventory, equipment, stats, loot |
| [`menu.md`](menu.md) | title, character creation, pause, bonfire menus |
| [`progress-ui.md`](progress-ui.md) | Estus, souls, bloodstain, death/respawn, HUD, text |
| [`save.md`](save.md) | save format, `sceUtilitySavedata` A/B slots, file fallback |
| [`asylum.md`](asylum.md) | how the modules are assembled into the first area |
| [`firelink.md`](firelink.md) | Firelink Shrine zone |
| [`game.md`](game.md) | the full game executable `ash_ds`: title -> Asylum -> Firelink |
