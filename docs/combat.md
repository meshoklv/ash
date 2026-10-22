# Боёвка DS1 в Ash: модуль `src/combat`

*14.10.2026, доводка 15.10 — §6. Код: `src/combat/` (C99, без PSP-заголовков), кукер `tools/cook_combat.py` (+ `tools/tae.py`), тесты `tests/combat/` (ПК), проверка хитбоксов по анимации `tools/combat_pose_check.py`. Первоисточник плана — combat-ai notes §4–5.*

Пометки: **[проверено]** — команда/тест с выводом ниже; **[данные]** — число прочитано из файлов копии DSR; **[версия]** — правило не найдено в данных, взято из вики/сообщества или придумано, калибруется «золотыми трассами» (combat-ai notes §6); **[придумал]** — решение без опоры.

## 1. Что сделано

| Механика | Откуда | Статус |
|---|---|---|
| Хитбоксы атак | TAE ev.1/2/5/304 → BehaviorParam(_PC) → AtkParam (капсулы между dummy poly, до 4 на атаку) | [данные], 0 неразрешённых поведений на 5 моделях |
| Хёртбоксы | капсулы между костями (11 штук: торс, шея-голова, руки, ноги) | радиусы [придумал]; DS1 использует Havok-формы, их не читаем |
| Урон | AR оружия (CalcCorrectGraph, ReinforceParamWeapon) × % AtkParam, по элементам; формула атака/защита сообщества | AR [данные]+[версия: формула AR]; формула урона [версия] |
| Стамина | цена атак — BehaviorParam.stamina; реген 45/с; реген по TAE 225 (0% в атаке, 20% в блоке) | цена и окна [данные]; 45/с и 91 при END 10 [версия, вики]; цена переката/бэкстепа 8 [версия, не найдена] |
| i-frames | TAE ev.0 флаг 8/94 (и 67/68 «кроме бросков») | [данные], ролл a00_0710 = 11 кадров [проверено] |
| Пойз и стаггер | броня: сумма Poise (46 у сета Элитного рыцаря); пойз-урон = BasePoiseDamage × %; гиперармор = TAE флаг 24 | числа [данные]; сброс через 5 с и «гиперармор = иммунитет к стаггеру» [версия] |
| Реакции | по AtkParam.dmgLevel: ≤1 малая, 2 средняя, 3–5 большая, ≥6 сбивание с ног | сопоставление [версия]; id анимаций — из TAE |
| Блок щитом | Knight Shield: срез 100/30/60/40 %, стабильность 56; потеря стамины = стам-урон × (1 − стаб./100); угол ±75° | срез и стабильность [данные]; формула и угол [версия] |
| Парирование | парирующий бокс a48_5100 (AttackType 64, кадры 0–7, dummy 131) против окна «можно парировать» атакующего (TAE флаг 5, у пустого a3000 — кадр 28) | [данные] |
| Рипост | окно SpEffect 30 «パリィ崩され状態（投げ条件）» в a2100 пустого (кадры 0–55); ThrowParam 22500: дист. 1,5 м, разворот 90–180°, атакующий a23_3201, жертва a2510; урон в окне e304 (кадр 25), AtkParam 2300510 = 250 % | [данные] |
| Бэкстаб | ThrowParam 12500: дист. 1,5 м, ±60° разворот, атакующий a23_3400, жертва a2500, AtkParam 2300500 = 200 % | [данные] |
| Смерть | hp ≤ 0 → событие, анимация смерти (игрок a00_6000, пустой/демон a2200) | id [данные] |
| Отбрасывание | AtkParam.knockbackDist × TAE 226 (% у атакующего) за 0,2 с, в блоке ×0,5 | дистанция [данные]; время и ×0,5 [версия] |
| Хит-стоп | AtkParam.hitStopTime (0,08 с у R1) — заморозка анимации у обоих | [данные] |
| Снаряды/волны | ev.2 → Bullet → BulletOnHit → AtkParam; сфера у dummy, растёт до FinalHitRadius за RadiusIncreaseTime | [данные]; без полёта [версия] |
| Уровни NPC | постоянные SpEffect «レベルN» из NpcParam.spEffectID0..7: HP/стамина/атака/защита × множители | [версия], сходится с вики: Демон-отшельник 2100 × 2,5 = 5250 |

**Не сделано (на 15.10, после доводки §6):** блок у NPC (у полых Прибежища щитов нет), яд/кровь, урон по стамине врага, магия/луки, сопротивление по типам (рубящий/дробящий/колющий — поля есть, не применяются), градиенты окон по весу (TAE 300 — берём окна для веса 1), AR двуручного хвата (+50 % силы не пересчитывается — см. §6), реакция Демона на плюх (a2520 — у Демона свой рантайм `src/boss`), буфер ввода и правило отмен (флаги отмен в `flags` есть, решает игра). Отскок оружия от щита и плюх — **сделаны в §6**.

## 2. Данные (`combat.ashcb`, вне git)

`tools/.venv/bin/python tools/cook_combat.py --out-dir ~/ash-assets/cooked` → `combat.ashcb` (77 КБ), `combat.json` (всё, что вошло, человекочитаемо), `knight_combat.ashanim` (17 клипов). 56 с на сборочной машине [проверено].

- Модели (kinds): `c0000` игрок (a00: 0–5, 100–164, 680–743, 1600–1610, 2000–2101, 6000–6084; a23 все; a48 все), `c2500` пустой, `c2550` тяжёлый пустой, `c2230` Демон-отшельник (Stray Demon, яма под ареной), `c2232` **Демон Прибежища** (NpcParam 223200, ИИ 223200_battle — именно он в бою, а не c2230). Итого 760 анимаций, 241 окно атак, 138 строк AtkParam [проверено выводом кукера].
- Экипировка игрока: класс Рыцарь (CharaInitParam 3001: VIT 14, END 10, STR/DEX 11) — только статы; оружие — то, что рисует кукер рыцаря: Длинный меч 201000 (AR 86,94: база 80 + коррекция 8,5 % по STR и DEX), Рыцарский щит 1451000; броня — сет Элитного рыцаря 350000–353000 (защита 95/42/45/35, пойз 46). HP 572 при VIT 14 [версия, вики].
- Формулы ID: игрок 100000000 + V·1000 + J, NPC 200000000 + V·1000 + J, common (ev.5) = J напрямую; V — BehaviorVariationID оружия (a23 → правое, a48 → левое) или NpcParam.
- Флаги TAE → биты `CB_ST_*`: таблица `FLAG_BIT` в кукере, имена — шаблон DSAnimStudio `TAE.Template.DS1.xml` (github Meowmaritus/DSAnimStudio, master).
- TAE DSR — версия 0x1000B с 32-битными смещениями; reader soulstruct читает только 0x1000C → свой `tools/tae.py` [проверено на a23.tae]. (Параллельно появился `tools/tae_ds1.py` агента босса — тот же формат; объединить при случае.)

Ключевые числа из `combat.json` (все [данные]):

| | |
|---|---|
| R1 длинного меча a23_3000 | окно 16–20, стамина 20, AtkParam 2300000: капсула dummy 100→110 r 0,4 + сфера 120 r 0,2, 100 %, dmgLevel 1, отбрасывание 0,72 м, хит-стоп 0,08 с |
| R2 a23_3300 | окно 22–26, стамина 45, 135 %, пойз 120 % |
| Перекат a00_0710 | i-frames 0–11 (флаг 8), реген 0 % кадры 0–41 |
| Пустой 250022 | HP 69, защита 77, пойз 0, атака a3000: окно 29–33, парируемо с кадра 28 |
| Демон Прибежища 223200 | HP 813, защита 83, a3000: окна 51–54 и 53–56, физ. 190, dmgLevel 6 |

## 3. Интерфейс для игры

```c
#include "combat.h"          /* src/combat; цель CMake ash_combat */

/* загрузка (один раз на зону) */
static CbData cbd; static CbWorld cbw; static CbActor player, enemy[16];
cb_data_load(&cbd, file_buf, size);              /* буфер живёт всю зону, выровнен на 4 */
cb_world_init(&cbw, &cbd);
cb_actor_init_player(&player, &cbd);             /* HP/стамина/пойз/оружие из данных */
cb_actor_bind(&player, &cbd, chr_bone_lookup, &knight_chr);  /* имена костей .ashc -> индексы */
cb_world_add(&cbw, &player);
cb_actor_init_npc(&enemy[i], &cbd, msb_npc_param_id, "c2500");   /* или "c2232" для Демона */
cb_actor_bind(&enemy[i], &cbd, chr_bone_lookup, &hollow_chr);
cb_world_add(&cbw, &enemy[i]);

/* каждый шаг 1/30 с, после анимации */
a->pos = ..; a->yaw = ..;                         /* model = T(pos)*Ry(yaw), как в src/game */
a->bones = (float *)world_mats; a->n_bones = n;   /* anim_skin_matrices(... world ...): МОДЕЛЬНОЕ пространство */
a->anim_id = clip_anim_id; a->anim_t = t;         /* id как в .ashanim: a23_3000 -> 233000, у NPC 3000 */
player.guard_input = held_L1 && !attacking;
combat_update(&cbw, DT);
for (k = 0; k < cbw.n_ev; k++) switch (cbw.ev[k].type) { /* CB_EV_HIT/BLOCK/PARRY/STAGGER/DEATH/THROW... */ }
if (a->req_anim) { play(a, a->req_anim); a->req_anim = 0; }   /* реакция: принудительный переход */
if (a->hitstop > 0) /* не двигать время анимации */;
if (a->push_t > 0)  /* сдвинуть на a->push * DT через коллизию */;

/* действие игрока по вводу */
if (r1_pressed) { if (cb_try_throw(&cbw, player_idx) < 0 && cb_begin_anim(&cbw, &player, 233000)) play(&player, 233000); }
if (roll_pressed && cb_begin_anim(&cbw, &player, 710)) play(&player, 710);
```

- `cb_try_throw` сам ставит обеим сторонам `req_anim` (игрок 233201/233400, жертва 2510/2500) и переносит жертву к точке dummy 230/231 атакующего; урон — когда анимация броска дойдёт до кадра окна e304.
- `CbActor.flags` — состояние текущего кадра (`CB_ST_INVINCIBLE`, `CB_ST_CANCEL_*`, `CB_ST_NO_TURN`, `CB_ST_RIPOSTABLE`…): игре — для отмен и поворота, UI — для лок-она (`CB_ST_NO_LOCKON`).
- Отладка: `combat_debug_shapes()` отдаёт капсулы хёрт/хит для отрисовки линиями.
- Если скелет у актора не передан (`bones = NULL`) или кость не найдена, точки берутся из позы связывания (bind pose) — так работают тесты на ПК.
- Анимации для игрока: `knight_combat.ashanim` — второй `AnimSet` того же скелета (`anim_load` + `anim_find` по имени: guard, guard_hit, guard_hit_l, guard_break, attack2, attack3, heavy, heavy2, parry, riposte, backstab, hit_s, hit_m, hit_l, knockdown, parried, death). Кукер рыцаря не менялся.
- Босс (`src/boss`, другая задача) может отдавать боёвке себя как `CbActor`: `bones = boss.world`, `anim_id = boss_anim_id()`, `anim_t = boss.t` (см. `boss.h`).

## 4. Проверки

`tests/combat/run.sh` — собирает тест хост-gcc из `~/opt/buildtools` с ASan/UBSan и гоняет на настоящем `combat.ashcb` (позы — bind pose, анимация костей не участвует; проверяются правила, окна и числа). Вывод 14.10 [проверено]:

```
1. Longsword AR 86.94 (base 80), R1 window frames 16-20, 100% phys -> hollow (def 77, hp 69): 39.04 dmg, poise 20.0, 2 hits to kill
   pipeline: 1 hit(s) at frame 16, hp 69.00 -> 29.96, stagger events 1, hollow anim 2000
2. roll a00_0710: invincible frames 11 (0..10) at 30 Hz
   hollow a3000 (window 29-33) vs roll started at frame 27: invuln events 1, hits 0, hp 572 -> 572
   hollow a3000 (window 29-33) vs roll started at frame 0: invuln events 0, hits 1, hp 572 -> 530
3. stamina 91, R1 costs 20 -> 71; at frame 20 (TAE 225 regen 0% window) 71.0
   1 s idle later: 91.0 (regen 45/s)
   R1 spam from full: 5 allowed, then refused (stamina 0.0)
   1 s in the guard anim a00_0001 (TAE 225 = 20%) from 10: 19.0
4. block: hp 572.0 -> 572.0 (shield phys cut 100%), stamina 91.0 -> 82.2 (atk stamina 20, stability 56), react anim 100
   hit from behind while guarding: blocks 0, hp 572.0 -> 530.1
5. parry: hollow a3000 parry-possible (TAE flag 5) from frame 28, player parry a48_5100 window 0-7 started at 25 -> parry at frame 28, hollow anim 2100, ripostable 1
   riposte: victim 1, player anim 233201, hollow anim 2510, damage 162.1 at step 25, hp 69.0 -> 0.0, deaths 1
   backstab: victim 1, player anim 233400, hollow anim 2500
6. Asylum Demon a3000 (window 51-54, phys 190, dmgLevel 6) vs player (def 95, poise 46): hits 1, hp 572 -> 447.6 (exp 124.4), player anim 2014
   R1 on the demon (def 83, hp 813): 36.27 dmg -> 23 hits
   Stray Demon (c2230, NpcParam 223000) with its level SpEffect 7014: hp 5250, phys def 258, attack x2.40
7. player death: events 1, anim 6000, dead 1
36/36 checks passed
```

Сборка под PSP: `psp-cmake` + цель `ash_combat` собирается без предупреждений, `.text` 13,8 КБ + 1,4 КБ [проверено `psp-size`]. Производительность на PSP **не мерилась**.

Хитбокс по настоящей анимации: `tools/combat_pose_check.py` строит скелет рыцаря в клипе a23_3000 тем же способом, что рантайм, и меч по dummy 100→110 (`~/ash-assets/shots/combat/a23_3000_hitbox.png`). Дотягивается до пустого в 0,8–1,6 м на кадрах 16–17, на 2,0 м — нет; длина клинка между dummy 0,79 м [проверено]. Скорость кончика ~1,5 м за кадр → в рантайме есть проверка «заметания» между кадрами.

## 5. Открытые вопросы

- **HP Демона Прибежища:** NpcParam 223200 = 813, вики пишет 825 — расхождение не объяснено (уровневого SpEffect у него нет).
- Формула урона, стоимость переката, сброс пойза, угол щита, длительность отбрасывания — сверить «золотыми трассами» на настоящих данных игры (combat-ai notes §6).
- `hitSourceType` AtkParam: 0 = правое оружие, 1 = левое [версия]; парирующий dummy 131 нашёлся только на теле (`c0000`, без кости — в корне модели, 0,65 м перед грудью).
- Окна TAE длиннее клипов HKX (у a48_5100 события до кадра 60, клип 36 кадров) — флаги после конца клипа игра не увидит, это не мешает.

## 6. Доводка 15.10: отскок, плюх, атаки с разбега/переката, тест-EBOOT

Модуль доведён 15.10. Код `src/game` не менялся: что должен сделать интегратор — §6.4.

### 6.1. Что добавлено в модуль

| Механика | Как | Пометка |
|---|---|---|
| **Отскок от щита (repel)** | в блоке: сила отскока атаки < `DeflectOnBlock` щита → атакующему `req_anim` = реакция `CB_R_REPELLED` + событие `CB_EV_REPEL`, короткий хит-стоп. Сила: NPC = `AtkParam_Npc.GuardAttackPower` (полый 20, Демон 100), игрок = `DeflectOnAttack` оружия × `GuardAttackPercentage` % (длинный меч 20 × 100 %; плюх и прыжок — 9900 %, не отскакивают). Рыцарский щит `DeflectOnBlock` 50: **удары полого отскакивают, Демона — нет** | числа [данные]; правило сравнения [версия] (поле есть, формулы в данных нет) |
| Анимация отскока | a00_2050 (у игрока, полого, тяжёлого полого) | [версия]: выбрана по позе — рука с оружием отброшена вверх-назад, корпус отклонён, 56–69 кадров, без окон удара; имени в данных нет |
| **Плюх (атака в падении)** | три клипа: старт a23_3800 (окно 6–30), петля падения a23_3801 (окна 0–39), приземление a23_3810 (окно 0–5, стамина 52); окна AttackType 62. Модуль бьёт каждую цель **один раз за плюх** (`CbActor.plunge_hit`, сбрасывается первой анимацией без окна 62); событие `HIT` несёт `atk_type = 62` | окна, AtkParam 2300040 (сфера r 1,2 м у dummy 130, 145 % AR, dmgLevel 6) и 2300050 [данные] |
| Урон плюха | **из данных: 71,2 по Демону (защита 83), 74,5 по полому** [проверено тестом]. В DS1 плюх по Демону ощутимо сильнее; множителя в AtkParam/SpEffect нет (SpEffect 32 на окне плюха = только `ThrowCondition 4`) [проверено чтением PARAM]. Игра сейчас считает 4 × AR = 286 [версия игры] — решать интегратору/пользователю | [данные] / расхождение с DS1 [версия] |
| **Атака с разбега** a23_3500 | AtkParam 2300520: 110 %, dmgLevel 2, отбрасывание 1,44 м, стамина 25 | числа [данные]; роль клипа [версия: порядок строк AtkParam_Pc + root-motion 236 в TAE] |
| Прыжковая атака (бег + R2) a23_3600 | 2300030: 145 %, сфера r 0,7 у dummy 130, стамина 52 | роль [версия] |
| Атака с переката a23_3900 | 2300540: 90 %, окно 7–10, стамина 20 | роль [версия] |
| Тяжёлая атака a23_3300/3301 | была; проверена в тесте и EBOOT: 135 %, стамина 45, стаггер полого в a2004 | [данные] |
| Двуручный хват | только id анимаций: `cb_attack_anim(..., two_handed=1)` → a23_4xxx (окна и AtkParam 2300200… есть в данных). **AR двуручного (+50 % STR) не пересчитывается** — в `CbWeapon` нет места, нужен второй AR в кукере | частично |
| Стаггер/пойз | без изменений правил; проверено: игрок (пойз 46 сета Элитного рыцаря) падает в реакцию на **5-м** ударе полого (пойз-урон 10), полый (пойз 0) — от каждого удара | [проверено] |
| События броска | `CB_EV_THROW` раньше терялся (cb_try_throw писал событие, а следующий combat_update очищал список) — теперь попадает в список ближайшего combat_update | баг-фикс [проверено] |

API (combat.h):

```c
uint32_t cb_attack_anim(const CbWorld *w, const CbActor *a, int action, int two_handed);
/* action: CB_ACT_R1 / _R1_2 / _R1_3 / _R2 / _R2_2 / _RUN_R1 / _RUN_R2 / _ROLL_R1 / _PLUNGE / _PLUNGE_FALL / _PLUNGE_LAND;
   -> категория оружия * 10000 + YYYY (+1000 двуручный), 0 если такой анимации в данных нет */
CbEvent.atk_type      /* HIT/BLOCK: AttackType окна (CB_ATTACK_PLUNGE = 62) */
CB_EV_REPEL           /* attacker отскочил от щита victim; attacker->req_anim = 2050 */
CB_R_REPELLED         /* индекс реакции в CbKind.react[] (12) */
```

### 6.2. Данные (`tools/cook_combat.py`, вне git)

- `knight_combat.ashanim` (тот же скелет рыцаря): добавлены клипы `plunge` a23_3800, `plunge_fall` a23_3801 (петля), `plunge_land` a23_3810, `run_attack` a23_3500, `jump_attack` a23_3600, `roll_attack` a23_3900, `repelled` a00_2050 — итого 24 клипа (было 17). Старые имена не менялись.
- **Новый файл `hollow_combat.ashanim`** — второй AnimSet для `hollow.ashc` (скелет и порядок костей `tools/cook_enemy.py`): `hit_m` a00_2004, `hit_l` a00_2010, `knockdown` a00_2014, `repelled` a00_2050. Без него стаггер dmgLevel ≥ 2 у полого в игре падает на запасной `hit`.
- `combat.ashcb`: реакция `REPELLED` в `CbKind.react[12]` (c0000/c2500/c2550 = 2050, Демоны = 0); формат и версия файла прежние. Кук 79 с на сборочной машине [проверено].

### 6.3. Проверки

**Нативные тесты** `tests/combat/run.sh` — **67/67** (было 36) [проверено 15.10]; новые строки вывода:

```
8. c2500 a3000 (GuardAttackPower 20) on the Knight Shield (DeflectOnBlock 50): blocks 1, repels 1, attacker anim 2050
8. c2232 a3002 (GuardAttackPower 100) on the Knight Shield (DeflectOnBlock 50): blocks 1, repels 0, attacker anim 0
   landing a23_3810 stamina 91 -> 39
9. plunge on a hollow (def 77): hits 1 (AttackType 62: 1), damage 74.5 (AtkParam 2300040: 145% AR, dmgLevel 6), hp 69 -> 0
9. plunge on the Asylum Demon (def 83): hits 1 (AttackType 62: 1), damage 71.2 (AtkParam 2300040: 145% AR, dmgLevel 6), hp 813 -> 742
10. running R1 a23_3500: window 16-20, stamina 25, AtkParam 2300520 110%, dmgLevel 2 -> hits 1, 46.7 dmg (exp 46.7), hollow stagger 1 anim 2004
10. heavy R2 a23_3300: window 22-26, stamina 45, AtkParam 2300100 135%, dmgLevel 2 -> hits 1, 66.4 dmg (exp 66.4), hollow stagger 1 anim 2004
10. rolling R1 a23_3900: window 7-10, stamina 20, AtkParam 2300540 90%, dmgLevel 1 -> hits 1, 31.6 dmg (exp 31.6), hollow stagger 1 anim 2000
11. player poise 46, hollow poise damage 10 per hit: first stagger on hit 5 (anim 0), hp 362
```

(У демона a3000 — `IgnoreGuard` в AtkParam_Npc 223200, его не заблокировать щитом [данные]; для теста отскока взят a3002.)

**Тест-EBOOT `ash_cbtest`** (`src/combat/test/cbtest.c`, сборка `psp-cmake -B build/cbtest … && cmake --build build/cbtest --target ash_cbtest`, прогон `src/combat/test/shoot-cbtest.sh ~/ash-assets/cooked <out>`): верхний зал Прибежища (arena_nochars + пол из arena.pcol), Элитный рыцарь против полого c2500 и Демона c2232, только модуль боя + анимация, автопилот без пада. Сцены: блок + отскок → парирование → рипост → бэкстаб → бег + R1 и R2 → пойз игрока → плюх на Демона с высоты балкона (8,7 м). На кадре — красные линии хитбоксов активных окон и строка последнего события. Прогон в PPSSPPHeadless 15.10 [проверено, эмулятор]:

```
f38   EV BLOCK: hollow -> knight, dmg 0.0, stamina -8.8, anim 100     | EV REPEL: hollow -> knight, anim 2050
f187  EV PARRY: knight -> hollow, anim 2100
f195  R1 after the parry: cb_try_throw -> 1 (dist 0.60 m)  | EV THROW: anim 2510
f219  EV THROW_HIT: knight -> hollow, dmg 162.1 | EV DEATH
f470  R1 behind the hollow: cb_try_throw -> 1 | EV THROW: anim 2500
f494  EV THROW_HIT: knight -> hollow, dmg 119.9 | EV DEATH
f672  EV HIT: knight -> hollow, dmg 46.7 (running R1) | EV STAGGER anim 2004
f731  EV HIT: knight -> hollow, dmg 66.4 (heavy R2)  | EV DEATH anim 2200
f848…f1093 EV HIT: hollow -> knight, dmg 41.9 x5 | EV STAGGER: hollow -> knight, anim 2000 (5-й удар)
f1146 R1 while falling: plunge a23_3800 (y 206.24)
f1172 EV HIT: knight -> demon, dmg 71.2, atk_type 62, victim hp 741.8
f1179 landed (fall 1.30 s, 13.1 m/s): plunge_land a23_3810, stamina 39.0
done hits 8 blocks 1 repels 1 parries 1 staggers 2 deaths 3 throws 2 throw_hits 2 plunge_dmg 71.2
```

Кадры (вне git): `~/ash-assets/shots/combat-test/final/` — `1_block_repel`, `2_parry`, `2_riposte_start`, `2_riposte_hit`, `3_backstab_hit`, `4_run_attack_stagger`, `4_heavy_kill`, `5_player_stagger`, `6_plunge_start`, `6_plunge_hit` (.png) + `cbtest.log`. На кадрах плюха рыцарь мелкий и тёмный на фоне Демона (камера поставлена лучом по arena.pcol, где не мешают колонны) — попадание видно по хитбоксу и строке события. Скорость эмулятора — не перф железа.

### 6.4. Что должен сделать интегратор в `src/game` (патч не применён)

1. **Клипы полого:** загрузить `hollow_combat.ashanim` вторым `AnimSet` (как `kcmb` у рыцаря) и в ветке реакций полого (`asy_game.c`, «reactions from combat», ~стр. 709) искать `clip_id` и в нём — тогда стаггер a2004/a2010, нокдаун a2014 и отскок a2050 играют свои анимации, а не запасной `hit`. Скопировать файл в стейдж/дистрибутив (`make-dist.sh`, `shoot-asylum.sh`).
2. **Отскок:** в разборе событий `step_combat` добавить `case CB_EV_REPEL:` — искры (`fx_push(g, 2, …)`), лог; полому прервать текущий EzState, как при парировании (`if (ve && ve->ai_ok) ve->ez = -1;` — здесь отскочивший = `ev->attacker`). `req_anim` атакующего (2050) модуль уже поставил.
3. **Плюх через модуль** (вместо заглушки `boss_take_hit(… 4 × AR …)` в «falling and the plunge attack», ~стр. 485–517):
   ```c
   /* R1 в падении */
   u32 id = cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE, 0);
   if (id && cb_begin_anim(&g->cbw, &g->pact, id)) body_play(&g->pb, clip_name(&g->kcmb, "plunge"), 0.05f, 1);
   /* клип кончился, ещё в воздухе */
   if (cb_begin_anim(&g->cbw, &g->pact, cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE_FALL, 0)))
       body_play(&g->pb, clip_name(&g->kcmb, "plunge_fall"), 0.05f, 1);
   /* приземление */
   if (cb_begin_anim(&g->cbw, &g->pact, cb_attack_anim(&g->cbw, &g->pact, CB_ACT_PLUNGE_LAND, 0)))
       body_play(&g->pb, clip_name(&g->kcmb, "plunge_land"), 0.05f, 1);
   ```
   В `step_combat` для `P_PLUNGE` передавать настоящий `anim_id` (сейчас обнуляется только `P_FALL` — так и оставить). В обработчике `CB_EV_HIT` по Демону: `boss_take_hit(&g->B, dmg, 20, ev->atk_type == CB_ATTACK_PLUNGE ? BOSS_HIT_PLUNGE : BOSS_HIT_NORMAL)`. Хёртбоксы Демона берутся из `g->bact` (bones = `g->B.world`), окно плюха — сфера r 1,2 м: попадание, если рыцарь падает в пределах ~2,3 м от центра Демона (в EBOOT при 3,5 м — промах) [проверено EBOOT]. **Урон из данных 71 против 286 у заглушки** — если бой с Демоном после этого слишком долгий, множитель ставить в игре с пометкой [версия].
4. **Атаки с разбега/переката:** в ветке `ACT_LIGHT`: если `p->st == P_RUN` → `cb_attack_anim(…, CB_ACT_RUN_R1, 0)` и клип `run_attack`; если идёт перекат и `pact.flags & CB_ST_CANCEL_RH` → `CB_ACT_ROLL_R1`, клип `roll_attack`. В `ACT_HEAVY` при беге → `CB_ACT_RUN_R2`, клип `jump_attack`. Все через `player_action`/`cb_begin_anim` (стамина из данных).
5. **Событие THROW** теперь приходит (раньше терялось): в логе игры появятся строки `throw: actor …` — обработчик уже есть, менять нечего.
