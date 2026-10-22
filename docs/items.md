# Предметы и инвентарь (src/items)

**На 15.10.2026.** База предметов из PARAM/FMG русской DSR, инвентарь со стаками и пределами, слоты снаряжения DS1, смена вещей с пересчётом веса/защиты/атаки, использование предметов, подбор лута по ItemLot с текстом «Получено: …», стартовый набор по классу (CharaInitParam), сейв. Пометки: **[проверено]** — командой/тестом, **[версия]** — гипотеза или число по памяти, **[придумал]** — наше решение.

В игру **не встроено** (интеграция — у E, рецепт ниже). Проверено нативными тестами на ПК (137 проверок) и тест-EBOOT `ash_itemstest` в PPSSPPHeadless (меню снаряжения/инвентаря/статуса на реальных данных).

## Файлы

| Путь | Что |
|---|---|
| `src/items/items.h` / `items.c` | формат `items.ashit`, API `itm_*`: база, инвентарь, снаряжение, статы, использование, лут, сейв. C99 без PSP-заголовков |
| `src/items/items_menu.h` / `.c` | мост в `src/menu`: `itm_menu_fill` заполняет `MenuGameView` реальными данными вместо заглушки `menu_fill_view_from_char` |
| `src/items/menu-ext-images.patch` | патч к `src/menu` (не применён, меню — чужая зона): иконки предметов из второго `.ashui` (`MENU_IMG_EXT`, `menu_set_ext_images`) |
| `src/items/CMakeLists.txt` | `ash_items` (+ `ash_items_menu`, если есть `ash_menu`) |
| `src/items/test/` | `test_items.c` + `run.sh` — нативные тесты на настоящем куке |
| `src/items/test_psp/` | тест-EBOOT `ash_itemstest` (`main.c`, свой `CMakeLists.txt`, патч меню накладывается на копию в build-каталоге), `shoot.sh` |
| `tools/cook_items.py` | → `items.ashit` + `items.ashui` + `items.json` |

Кук — вне git (`~/ash-assets/cooked/items/`): `items.ashit` **51 460 Б**, `items.ashui` **220 928 Б** (107 иконок 32×32 CLUT8), ~9 с **[проверено]**.

```bash
cd ~/ash && . scripts/env.sh
nice -n 19 tools/.venv/bin/python tools/cook_items.py --out-dir ~/ash-assets/cooked/items [--png ~/ash-assets/shots/items]
src/items/test/run.sh                          # нативно: TEST:{"suite":"items","checks":137,"fail":0}
psp-cmake -S src/items/test_psp -B build/items-psp -G Ninja -DCMAKE_BUILD_TYPE=Release && nice -n 19 cmake --build build/items-psp
src/items/test_psp/shoot.sh ~/ash-assets/shots/items/run1      # emu-slot.sh + shm-shim, 10 PNG + items.log, ~20 с
```

## Что в базе (откуда)

- **Ключ предмета** = `cat << 28 | id` (0 оружие, 1 броня, 2 кольцо, 4 вещь) — **тот же, что в `src/event`** (`EvfLotItem.item`, `EV_CMD_ITEM c.a`, `need` у дверей) **[проверено: ключ от темницы 4<<28|2010 совпадает]**.
- **Набор предметов (107)**: всё, что дают лоты m18_01/m10_02, 10 классов (3000…3009), 9 даров (2400…2408), плюс фляга эстуса 200/201 и рукоять прямого меча 212000.
- **Лоты (84)**: сокровища MSB обеих карт (без `宝_救済用` — «спасательный» сундук, как в `docs/firelink.md`), `AwardItemLot` (2003[36]) из EMEVD с явным id, подарок Оскара 1080, дроп Демона 2660/2661, `NpcParam.ItemLotID1…6` персонажей MSB (дроп врагов: полые c2500 — 98 «ничего» / 2 «Сломанный прямой меч» и т. п.). Нет в ItemLotParam: 1810020…1810050, 22320000 (как и у `cook_events.py`) **[проверено]**.
- **Имена** — FMG item 10…13, **описания** — длинные 24…27 (короткие 20…23 запасные); «Получено: …» — FMG menu 101/10010950, «Использовано: …» — 101/10010860 **[проверено]**. «Нельзя унести больше» — **[придумал]**.
- **Иконки** — тем же резаком, что `tools/cook_menu.py` (`item_icon`, атласы Icon*), в контейнере `.ashui` src/ui; лист превью `~/ash-assets/shots/items/items_icons.png` просмотрен глазами **[проверено]**.
- **Числа**: оружие — атака 4 вида, масштабирование STR/DEX/INT/FTH, требования, % блока, устойчивость, строка `CalcCorrectGraph`; броня — защиты, баланс, сопротивления; кольца — множители из их SpEffect (`MaxEquipLoadMultiplier`, `MaxHPMultiplier`, …); вещи — что делает использование: души (`SpEffect.SoulAmountChange`: «Душа пропавшего мертвеца» = 200), человечность (`SpecialState 119`/`HumanityDamage`), лечение, бросок (`Bullet`), ключ (`GoodType 1`).
- **Вид оружия** (щит/лук/стрелы/катализатор…) — по диапазонам id **[версия]**: в PARAM DS1 поля «тип» нет.

## Логика

| Что | Как |
|---|---|
| Стаки | вещи и стрелы — до `MaxHoldQuantity` (человечность 99), лишнее не берётся (`itm_add` возвращает сколько взято) **[придумал: в DS1 излишек уходит в Бездонный ящик]**; оружие/броня/кольца — каждый экземпляр отдельной строкой |
| Предел | 64 строки инвентаря **[придумал]** |
| Эстус | одна фляга (200 пустая / 201 полная — один предмет); заряды — у `src/progress` (`Progress.estus`), инвентарь их только показывает через `inv->estus`; `itm_use` → `ITM_USE_ESTUS`, ничего не списывает |
| Слоты | `ITM_SLOT_*` = порядок `MSLOT_*` меню: R1 R2 L1 L2, стрелы ×2, болты ×2, голова/торс/руки/ноги, кольца ×2, быстрые ×5. Проверка слота: правая/левая рука по `RightHandAllowed/LeftHandAllowed`, броня по `EquippedTo*`, быстрые — вещи с `CanBeEquipped`. Одно и то же кольцо дважды нельзя (DS1). Перенос вещи из слота в слот — обмен (DS1) |
| Статы (`itm_calc`) | вес = руки + стрелы + броня + кольца; предел = 40 + END **[версия]** × кольцо; перекат: ≤25 % быстрый, ≤50 % средний, ≤100 % тяжёлый, выше — перегруз (DS1); баланс/защита = броня + кольца (без базы от уровня **[версия]**); HP по VIT и энергия по END — таблицы **[версия: опорные точки по памяти; 14→572 и 8…12 совпадают с `cook_combat.py`]** |
| Атака (`itm_weapon_ar`) | AR = база + база × масштаб% × CalcCorrectGraph(стат)% — та же формула, что в `cook_combat.py` **[проверено: Длинный меч у Рыцаря 86,94 = 86,94]**; двуручный хват — STR ×1,5 (DS1); не хватает требований — без масштабирования × 0,6 **[версия]** |
| Использование | души → `ITM_USE_SOULS` (игра: `prg_add_souls`), человечность → `inv->humanity += n` (+ полное лечение — дело игры), бомба → `ITM_USE_THROW` с id Bullet, ключи/материалы — нельзя (`ITM_ERR_USE`); расходуемые списываются по 1 |
| Лут | лот с «пустой» строкой, у которой есть очки шанса (дроп врага), → один предмет взвешенным выбором (`xorshift32`); остальные лоты (сундуки, трупы) → все предметы **[версия]**; флаг ItemFlag ведёт `src/event` |
| Старт | `asylum=1` — старт DS1 в Прибежище **[версия по памяти]**: броня класса, рукоять прямого меча в R1, без щита и эстуса, вещи класса (Знак Тьмы, Чёрный кристалл разделения); оружие класса лежит у костра 2 (лоты 1810100…1810330), эстус — от Оскара. `asylum=0` — полный набор CharaInitParam + эстус ×5 **[версия]** |
| Сейв | 555 Б, магия `ITMS`, версия 1; ключи сверяются с базой при чтении |

Вес полного стартового набора всех 10 классов совпал с весом в `menu.json` (`cook_menu.py`): 21,4 / 31,0 / 11,6 / 12,1 / 14,9 / 14,3 / 9,0 / 10,0 / 12,0 / 4,5 **[проверено тестом]**.

## Подключение к игре (для интегратора, E)

1. Корневой `CMakeLists.txt`: `add_subdirectory(src/items)` после `src/menu`; игре — `target_link_libraries(... ash_items ash_items_menu)`. Рядом с EBOOT — `items.ashit`, `items.ashui`.
2. Иконки в меню: `git apply src/items/menu-ext-images.patch` (≈20 строк в `menu.c`/`menu.h`: `MENU_IMG_EXT`, `menu_set_ext_images`, `bind_img` берёт картинку из второго ресурса). Без патча меню просто не рисует иконки предметов. Патч проверен: накладывается на `src/menu` из `main` (`09b6000`) в `src/items/test_psp/CMakeLists.txt` **[проверено]**.
3. Код (в `asy_game.c`/`asy_main.c`):

```c
#include "items.h"
#include "items_menu.h"
static ItmDb idb; static Inv inv; static UiRes item_icons; static ItmStats ist; static ItmMenuMap imap;
/* загрузка */
n = ash_load_file("items.ashit", &f, 0, 0); itm_db_load(&idb, f, n);
n = ash_load_file("items.ashui", &f, 0, 0); if (!ui_load(&item_icons, f, n)) menu_set_ext_images(&menu, &item_icons);
/* новая игра (MEV_NEW_GAME): */
itm_new_game(&inv, &idb, menu.chr.class_row, menu.chr.gift_idx ? 2400 + menu.chr.gift_idx : 0, /*asylum*/ 1);
inv.estus = &g->prg.estus;
/* EV_CMD_ITEM (сейчас только тост и g->items++): */
case EV_CMD_ITEM: {
    int k = itm_add(&inv, &idb, (uint32_t)c->a, c->b);
    ItmGot got = {(uint32_t)c->a, c->b, k};
    itm_got_text(&idb, &got, 1, g->toast_buf, 96);      /* «Получено: Палаш»; 96 Б = одна строка HUD */
    g->toast_text = g->toast_buf; g->toast_t = 0; ...
}
/* дверь с ключом: src/event сам проверяет ev_has_item (у него своя копия выданных ключей); чтобы единым
   источником был инвентарь - в event.c заменить ev_has_item на колбэк игры -> itm_has(&inv, key) (зона E) */
/* эстус: asy_has_estus -> itm_has(&inv, ITM_KEY(4,200)) || itm_has(&inv, ITM_KEY(4,201)) */
/* быстрый предмет: крестовина вниз -> itm_quick_next(&inv); кнопка предмета -> */
ItmUse u;
if (itm_use_quick(&inv, &idb, &u) == ITM_OK) switch (u.kind) {
    case ITM_USE_ESTUS:    prg_try_estus(&g->prg); break;
    case ITM_USE_SOULS:    prg_add_souls(&g->prg, u.value); break;
    case ITM_USE_HUMANITY: /* полное лечение HP */ break;
    case ITM_USE_THROW:    /* огненная бомба: Bullet u.value (опционально) */ break;
}
/* смена оружия -> / <- : itm_switch_right / itm_switch_left; новое оружие в руке: itm_right_weapon(&inv) ->
   перезагрузить клипы/хитбоксы src/combat (cbdata ищет EquipParamWeapon по id = ITM_KEY_ID(key)) */
/* меню: перед menu_open(PAUSE/INVENTORY/EQUIP/STATUS) и после каждого изменения: */
itm_calc(&inv, &idb, view.attr, &ist);
itm_menu_fill(&idb, &inv, &ist, &view, &imap);          /* имя/класс/уровень/души/атрибуты - как раньше */
case MEV_USE_ITEM: { int idx = imap.inv_idx[menu.ev_arg]; ItmUse u; itm_use(&inv, &idb, idx, &u); /* как выше */ }
/* сейв: рядом с prg_save_write/ev_save_write - itm_save_write(&inv, buf, cap) (555 Б) / itm_save_read */
```

4. **Для src/menu (C)**: экраны снаряжения и инвентаря — только просмотр. Для выбора вещи в слот есть `itm_candidates(&inv, &idb, slot, out, max)` (индексы инвентаря, подходящие в слот) и `itm_equip(&inv, &idb, slot, idx)` / `itm_unequip`; нужен экран списка и событие вроде `MEV_EQUIP(slot, idx)` — это правка меню.

## Тест-EBOOT `ash_itemstest` (PPSSPPHeadless)

Рыцарь стартует «по-прибежищному», подбирает лут через `itm_award_lot`, меняет снаряжение, использует «Душу пропавшего мертвеца» из инвентаря (событие меню `MEV_USE_ITEM` → `itm_use`), фон — зал Прибежища и модель класса. Кадры `~/ash-assets/shots/items/run1/*.png`, лог `items.log` **[проверено в PPSSPP, кадры просмотрены]**:

| Кадр | Что видно |
|---|---|
| `01_equip_asylum_start` | рукоять прямого меча в правой руке, левая пуста, броня рыцаря, быстрые: Чёрный кристалл, Знак Тьмы; AR 20, загруженность 26/50 |
| `02_got_cell_key` | «Получено: Ключ от темницы в подземелье» |
| `04_equip_broadsword_tower_shield` | Палаш R1, Башенный щит L1, рукоять R2; AR 89, лев. 66, загруженность 32/50 |
| `05_got_firelink_loot` | «Получено: Ржавое железное кольцо…» (5 лотов одной строкой, обрезано целыми словами) |
| `06_equip_ring_quick` | кольцо в слоте, быстрые: эстус 5, бомбы 6, человечность 3 |
| `07_inventory`, `09_inventory_soul` | список с иконками и количеством, описание FMG справа |
| `10_after_use` | «Использовано: Душа пропавшего мертвеца», души 1200 → 1400, строка исчезла |
| `11_status`, `13_status_hammer_r2` | статус; после Демонического молота в R2 — загруженность 53,0/50,0 (перегруз) |

Память (newlib-куча в эмуляторе): меню + `items.ashit` + `items.ashui` = 750 КБ после загрузки (из них items ≈ 272 КБ файлов). **Эмулятор ≠ железо**: время и память PSP не мерились.

## Не сделано

- Не встроено в игру (рецепт выше); патч меню не применён к `src/menu`.
- Выбор вещи в слот из меню (нужен экран у C), сортировка, выброс, Бездонный ящик.
- Улучшения (+1…+15, `ReinforceParam`), прочность, заклинания/слоты настройки, сопротивление статусам в бою.
- Огненная бомба: модуль отдаёт id Bullet, полёт/взрыв — не сделаны.
- База защиты от уровня/атрибутов, точный штраф за требования, таблицы HP/энергии — **[версия]**, сверить с игрой.
