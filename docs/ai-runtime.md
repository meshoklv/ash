# ИИ врагов на PSP: как исполняем скрипты DSR

*14.10.2026. Веха M4, часть 2 («враг и его мозг»). Факты на копии DSR — ai-facts notes, общий план ИИ — combat-ai notes §3, решение — decisions notes.*

## Решение: (а′) — стоковая Lua 5.0.3 исполняет **оригинальный байткод DSR**

Не декомпилируем-перекомпилируем и не транслируем в C. Байткод из `script/*.luabnd.dcx` грузится как есть:

- **На ПК (x86-64 Linux)** — без единой правки формата: заголовок DSR `1b4c7561 50 01 04 08 04 06 08 09 09 08` = Lua 5.0, LE, int 4, size_t 8, Instruction 4, double. Стоковая 5.0.3 на LP64 отличается только `Instruction` (там `unsigned long` = 8 Б), правка одной строки `typedef unsigned int Instruction;` — `tools/ai/build_lua50.sh` **[проверено: `luac -l` разбирает все 538 чанков; прототип ниже исполняет 20 нужных полому]**.
- **На PSP (Allegrex, 32 бит)** — тот же байткод, перепакованный `tools/ai/luac_repack.py`: меняются только заголовок, длины строк (size_t 8 → 4) и, по выбору, числа (double → float). Поток инструкций копируется побитно. Ридер проверен на всех 538 чанках: обратная запись в x64 даёт байт-в-байт исходник **[проверено]**. 32-битный float-вывод (`--number float --strip`) грузится и исполняется Lua 5.0.3, собранной psp-gcc, в PPSSPPHeadless **[проверено 14.10: `tools/ai/stage_psp.sh`]**.

### Почему не (б) «транслятор в C/таблицы» и не декомпиляция

1. **Декомпилятор врёт как раз в ядре.** DSLuaDecompiler (свежий, из soulstruct, под mono) на `common_battle_func`, `common_logic_func`, `REGIST_FUNC` выдаёт мусор вместо замыканий: `ai = goal`, `ai = defFuncParamTbl` (×20 подряд), `ai = isRunNonBattle`. Это функции, через которые проходит **каждое** решение полого (`Common_Battle_Activate` выбирает Act по весам, `_COMMON_EasySetup` ставит боевую цель). Скомпилировать такое обратно нельзя, руками чинить — значит переписывать ядро ИИ по догадке. В байткоде всё верно (`CLOSURE` + псевдо-`MOVE` апвэлью — штатная форма Lua 5.0) **[проверено: `.lst` против `.lua`]**.
2. **Порядок загрузки — тоже логика.** В `common_func2` функция `Approach_Act` определена дважды (стр. 537 и 692 декомпиляции, вторая с параметром `life`) — побеждает последняя. Транслятор должен повторить и это; VM повторяет даром.
3. **API маленький.** Полый 250000 со всем, что достижимо из него статически (99 Lua-функций, 15 Lua-целей), трогает 38 методов `ai:` и 7 методов `goal:` (список — `ai-facts notes` §4). Из них для арены значимы ~15, остальное — заглушки (команда, лестницы, отладка). Это не «сотни API движка» из `plan notes` §3.8 — та оценка была по всем 533 скриптам.
4. **Боссы и следующие враги — бесплатно.** Демон (`223200_battle`), Чёрный рыцарь, полые других карт — те же общие файлы и тот же хост; транслятор пришлось бы расширять на каждого.
5. **Цена по CPU мала** (оценка ниже): скрипты почти все `REGISTER_GOAL_NO_UPDATE`, Lua вызывается на Activate/Interupt и раз в «думку», покадрово работают нативные цели на C.

Запасной путь (б) остаётся: если на железе Lua не влезет в 1–2 мс/кадр на 4–6 врагов, лог прототипа (какая цель с какими параметрами на какой дистанции) — готовая спецификация для таблицы.

### Числа: float или double на PSP

`luac_repack.py --scan` по всем 538 чанкам DSR (aiCommon + 17 карт): 46 330 числовых констант, **целых больше 2^24 — ноль**; 153 дробных значения не точны во float32 (`1.7`, `0.1`, …; ошибка ~1e-7 относительная) **[проверено]**. Значит `LUA_NUMBER=float` безопасен для ID и флагов; дробные — это дистанции/время, где 1e-7 ничего не меняет **[версия]**. Решение: **float** (у Allegrex есть FPU одинарной точности, double — программный).

## Что лежит в репо

| Путь | Что |
|---|---|
| `src/ai/aihost.h`, `aihost.c` | **Хост ИИ**: дерево целей, нативные цели, методы `ai:`/`goal:`, глобалы `REGISTER_*`/`ENABLE_COMBO_ATK_CANCEL`. Чистый C99 + Lua 5.0 API, без `src/game`. Мир — только через `AiWorld` (колбэки дистанции, угла, движения, запуска EzState, лога) |
| `src/ai/aitest.c`, `build_proto.sh` | Прототип на ПК: заглушка мира, прогон полого, лог действий → `build/host/ai_proto` |
| `tools/ai/build_lua50.sh` | Lua 5.0.3 (lua.org, sha256 сверяется) → `$ASH_OPT/lua50` с правкой `Instruction` |
| `tools/ai/luac_repack.py` | Перепаковка байткода x64 → 32 бит (+float, +strip), скан констант |
| `tools/ai/extract_ai.py` | Извлечение `.luac` + декомпиляция (`.lua`) + листинг (`.lst`) в `~/ash-assets/ai/`, статическое замыкание API для цели |
| `tools/ai/think_export.py` | NpcParam + NpcThinkParam → `think_<npc>.txt` (значения `GetExcelParam`) |
| `tools/ai/esd_calls.py` | Какие ESD-функции/команды реально вызывает chr-ESD (факт (в)) |

Производные ассеты (`~/ash-assets/ai/`: `.luac`, `.lua`, `.lst`, `api_*.txt`, `think_*.txt`) — **не в git**.

## API хоста (для `src/game`, `src/boss`)

```c
AiHost *h = ai_host_new();                          // одна VM на процесс
ai_host_load(h, "…/aiCommon/ai_define.luac");       // порядок: ai_define, goal_list, goal_list_dlc, logic_list,
...                                                 // event_list, common_func, common_func2, common_battle_func,
ai_host_load(h, "…/m18_01_00_00/250000_battle.luac"); // common_logic_func, top_goal, 010000_logic, Lua-цели, battle
AiAgent a = {0};                                    // по одному на врага, память — у вызывающего
ai_agent_init(h, &a, &world, "common10000_Logic", "common10000_Interupt", seed);
a.excel[8] = 250000; …                              // GetExcelParam из think_export (или из своего PARAM)
a.battle_state = 1;                                 // игрок обнаружен (восприятие — за движком)
ai_agent_tick(h, &a, 1.0f / 30);                    // каждый кадр
ai_agent_interrupt(h, &a, 2 /*INTERUPT_Damaged*/);  // из боёвки: урон, промах, блок…
```

`AiWorld.ez_action(a, ez)` — главный стык с боёвкой: ИИ просит EzState (3000+ атака, 700–703 шаг), движок играет анимацию по TAE и возвращает её длину; конец анимации = успех `CommonAttack`. Номер EzState = номер анимации врага `a00_<ez>` (у полого `a00_3000…3009`, `ai-facts notes` §5) **[версия: связь через enemyCommon.esd `GetAIActionType`/`GetCommandIDFromEvent` не прослежена]**.

**Для босса (`src/boss`):** тот же хост, грузить `223200_battle.luac` (Демон Прибежища, ThinkParam 223200: battle 223200) вместо 250000 и реализовать в `AiWorld` его EzState. Нативные цели, которых нет у полого (TurnAround, Guard, …), сейчас заканчиваются сразу с пометкой `?? native goal … not implemented` в логе — их список даст прогон `ai_proto` с его скриптом. Дублировать хост не нужно.

**`REGIST_FUNC` подменён в хосте (C, `r_regist_func`).** У Демона Act15 и Act16 («вернуться домой», когда игрок вне региона 1812990) в оригинальном байткоде ничего не возвращают (`RETURN 0 1` после `GetWellSpace_Odds = 0`) **[проверено: `223200_battle.lst`]**, а `Common_Battle_Activate` (строка 133) сравнивает `rand(1,100) <= <возврат акта>` → на стоковой Lua 5.0 «attempt to compare number with nil» на каждой такой думке (87 строк за прогон route). Ошибка случалась уже после постановки всех подцелей, пропускала только AdjustSpace — то же, что шанс 0. Хостовый `REGIST_FUNC` превращает `nil` в `0`: поведение то же, ошибки нет **[проверено: `ai_proto` 223200 и 250000, сиды 1 и 7 — вывод старой и новой сборки совпадает построчно, ошибок 12/13 → 0]**.

## Семантика хоста — что из данных, что придумано

| Что | Откуда |
|---|---|
| Имена функций целей `<Name>_Activate/_Update/_Terminate/_Interupt`, флаги NO_UPDATE / NO_INTERUPT / UPDATE_TIME | данные (байткод) |
| `CommonAttack` (2200) — нативная, хотя в Lua зарегистрирована с пустыми функциями | данные + combat-ai notes §3.1 (версия «атаку делает движок») |
| Wait, Stay, MoveToSomewhere, Guard, SidewayMove, KeepDist, BackToHome, SpinStep, LeaveTarget, ApproachStep, Parry — `REGISTER_GOAL(id, name)` без функций → нативные | данные (`common_func.lua`) |
| Battle-цель ставит `_COMMON_AddBattleGoal`: `AddTopGoal(GetExcelParam(battleGoalID), -1)` | данные |
| Цель с подцелями: Update (если не NO_UPDATE) → первая подцель; провал подцели = провал родителя; успех последней = успех родителя | **[версия]** |
| Lua-цель без подцелей и без Update — успех сразу после Activate | **[версия]** |
| Истёк `life`: нативные — успех (CommonAttack — провал); Lua-цели с Update сами читают `GetLife()` | **[версия]** (`approach_target.lua` делает так) |
| «Думка»: когда у TopGoal нет детей, вызывается логика `common10000_Logic` | **[версия]** |
| Прерывание: сверху вниз по активной цепочке, пропуская NO_INTERUPT; первая `true` забирает | **[версия]** |
| ГСЧ: xorshift32 с сидом, `GetRandam_Int(a,b)` равномерно | **[придумал]** (ГСЧ игры не документирован) |
| Кинематика заглушки: шаг 1,5 м/с, бег 4 м/с, поворот 180°/с, EzState 1,2 с, шаг 0,6 с/2 м | **[придумал]**, только для прототипа |

## Прототип: результат

`tools/ai/build_proto.sh && build/host/ai_proto ~/ash-assets/ai 250000 1` — полый NpcParam 250022 / ThinkParam 250000 / battle 250000. Вывод детерминирован (две прогонки — одинаковый md5 без строки времени) **[проверено]**. Полный лог — в отчёте агента и `ai-facts notes` §6; главное:

```
== A: first action after the first think, 200 seeds per distance (goal 250000)
start_d  | first EzState chosen (count)                  | first step | mean dist at attack
   0.8 m | 3000:75 3003:55 3008:70                       |   0        | 0.80 m
   3.0 m | 3000:57 3003:41 3004:39 3008:63               |   0        | 1.73 m
   6.0 m | 3000:24 3003:18 3004:56 3005:41 3006:22 3008:39 |   0        | 3.50 m
  12.0 m | 3000:6 3003:25 3004:71 3005:57 3006:33 3008:8 |   0        | 4.58 m
== stats (x86-64 host, all runs): lua calls 47775, time in Lua 127.9 ms, worst call 269 us, goal pool peak 9, Lua heap 341 KB
== unimplemented methods called: 0
```

**Бюджет [оценка, не замер]:** 2,7 мкс на вызов Lua на Xeon E5-2695 v2; PSP 333 МГц медленнее в ~15–30 раз → ~40–80 мкс на вызов, ~10 вызовов на «думку» → 0,5–1 мс на думку одного врага, думки редкие (раз в 1–5 с на врага). Куча Lua 341 КБ — это все общие файлы в double/x64; на PSP float + strip + только нужные чанки — меньше. Цифры с железа — этап 6 `combat-ai notes`.

## Сборка под PSP — сделано

- `src/ai/CMakeLists.txt`: цели `ash_lua50` (ядро + base/table/string/math, `LUA_NUMBER=float`), `ash_ai` (хост), `ash_aitest` (EBOOT теста). Подключено в корневой `CMakeLists.txt`; без `$ASH_OPT/lua50/src` (`tools/ai/build_lua50.sh`) каталог пропускается с предупреждением. Для игры: `target_link_libraries(<цель> PRIVATE ash_ai)`.
- `tools/ai/stage_psp.sh`: сборка, перепаковка 20 чанков полого (596 КБ на диске с запасом — весь aiCommon), прогон в PPSSPPHeadless под `flock ~/ash-assets/.emu.lock` + shim.
- **Результат [проверено]:** вывод PSP-сборки (float, 32 бит, без отладочной информации) **построчно совпадает** с ПК-прототипом (double, x64): 97–98 строк, diff пуст (без строки времени). Куча Lua на PSP после всех прогонов — **189 КБ** с 20 чанками полого и **353 КБ** со всем aiCommon без чужих battle-скриптов (50 чанков, текущий `aitest`); на ПК 341/407 КБ. Время в эмуляторе (2,3 с на 47 775 вызовов) — **не время PSP** (эмулятор не равен железу).

## Что дальше (по порядку)

1. Замер на железе: мс на думку, куча; перф-строка `ash_aitest` уже печатает время по `sceRtcGetCurrentTick`.
2. Интеграция в `src/game` (не моя зона): `AiWorld` поверх контроллера врага, `ez_action` → проигрыватель TAE, прерывания из боёвки.
3. Восприятие (NpcThinkParam: `SightDistance` 30, `HearingDistance` 10, `BattleStartDistance` 20) → `battle_state`.
4. Сверка с DSR трассами (`combat-ai notes` §6): частоты Act по корзинам дистанции.
