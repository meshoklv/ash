# Конвейер ассетов DSR → PSP

Что есть в коде на 14.10.2026 **[проверено чтением `tools/*.py` и прогоном из README]** и куда он должен прийти по плану (plan notes §5). Реальные числа ассетов — stats notes (генерируется `tools/probe.py`).

## Где что лежит

| Что | Путь | В git? |
|---|---|---|
| Сырьё DSR (вся игра, 3,6 ГБ, включая `msg/RUSSIAN`) | `~/ash-assets/dsr` (или `$ASH_DSR`) | **никогда** |
| Кукнутые сцены `.ashm` + `.json` со статистикой | `~/ash-assets/cooked` | **никогда** |
| Скриншоты прогонов | `~/ash-assets/shots` | **никогда** |
| Код конвейера | `tools/*.py` | да |
| venv (Python 3.13, uv) | `tools/.venv` (289 МБ) | нет |

Откуда сырьё: копия DSR пользователя (своя установка, путь в `$ASH_DSR`).

## Окружение

```bash
uv venv -p 3.13 tools/.venv && VIRTUAL_ENV=tools/.venv uv pip install \
  "soulstruct @ git+https://github.com/Grimrukh/soulstruct" "soulstruct-havok @ git+https://github.com/Grimrukh/soulstruct-havok" \
  pillow numpy pyfqmr colorama
```

- soulstruct / soulstruct-havok требуют Python ≥3.13 **[проверено по pyproject]**; scipy тянет soulstruct-havok.
- Всё — чистый Python на Linux, без Blender, Windows и texconv.
- **Запрещено:** `to_spline_hkx` и `convert_hk` из soulstruct-havok — вызывают Windows-бинарь `CompressAnim.exe`. Нужен только обратный путь spline → значения (`to_interleaved_container`), он на чистом Python.

## Что делает код сейчас (кук v0)

| Файл | Роль |
|---|---|
| `tools/dsrlib.py` | Чтение DSR через Soulstruct: DCX/BND/split-BXF (tpfbhd/bdt), FLVER (LOD0, strips разворачиваются, вырожденные треугольники выкидываются), TPF → DDS → Pillow, индекс текстур области карты, текстуры персонажа |
| `tools/probe.py` | Статистика по m18_01: MSB (части, персонажи, точка старта), куски карты, текстуры, персонажи, части рыцаря → the stats design notes |
| `tools/pose.py` | Поза персонажа: один кадр HKX-клипа (spline декодируется soulstruct-havok), скиннинг **на CPU во время кука**. Кости FLVER без дорожки в HKX следуют за родителем с bind-offset |
| `tools/ashm.py` | Обработка мешей и запись `.ashm` v1: слияние батчей, сварка, упрощение **pyfqmr** (quadric) + перепроекция UV, дробление рёбер длиннее порога (GE клипает только по near-плоскости), разбиение под u16-индексы, текстуры → **CLUT8 + swizzle** (16 Б × 8 строк), степень двойки, ≤128 (персонажи/босс до 256) |
| `tools/cook_scene.py` | Сцена `hall`: куски m18_01 `m0300B1 m0301B1 m0310B1 m0320B1` по трансформам MSB + Элитный рыцарь (HD/BD/AM/LG 9500, меч 0201, щит 1516, поза `c0000_a00_lo/a00_0000`) + Демон Прибежища c2230 (поза `a00_0000`). Лайтмап карты (UV1) запекается в цвет вершин, персонажам — простой ламберт. Материалы `Water/_Edge/Fly/Snow/Light/Shadow/Mirror` пропускаются. FromSoft-пространство левостороннее → в конце зеркалится X (x → −x), обход граней становится CCW |

Формат `.ashm` v1 описан в docstring `tools/ashm.py` (заголовок, текстуры CLUT8 с палитрой ABGR8888, батчи с флагами alpha-test/no-cull и ограничивающей сферой, вершина 24 Б `float uv, u32 цвет, float xyz`, индексы u16, камеры). Это **формат вьюера для проверки конвейера**, не финальный `.pak` из плана.

Результат на 14.10 (из `~/ash-assets/cooked/hall.json`, см. также `stats notes`): карта 91 424 → 34 918 треуг., рыцарь 15 312 → 4 219, Демон 21 801 → 5 021; файл 2,7 МБ, 44 158 треуг., 62 батча, 21 текстура (397 КБ CLUT8). Для сравнения: план держит ≤20–22 тыс. **видимых** треуг. в кадре и персонажа ≤2,5 тыс. — кук v0 бюджеты ещё не соблюдает.

## Команды

```bash
tools/.venv/bin/python tools/probe.py                       # -> the stats design notes
tools/.venv/bin/python tools/cook_scene.py --scene hall --out ~/ash-assets/cooked/hall.ashm
#   --pose <anim>        клип c0000 для рыцаря (пусто = bind pose)
#   --demon-pose <anim>  клип c2230 для Демона
#   --map-budget N       бюджет треуг. карты (0 = без упрощения)
#   --no-chars           только карта
#   --debug-marker       красный куб в 3 м по +X FromSoft от рыцаря (проверка зеркалирования)
. scripts/env.sh && psp-cmake -B build -G Ninja && cmake --build build
scripts/shoot-viewer.sh ~/ash-assets/cooked/hall.ashm ~/ash-assets/shots/<run> "0 1 2 3" 4
```

Ключи `cook_scene.py` меняются по мере работы других участников — актуальное в `--help`.

## Куда идёт (план, `plan notes` §5)

Две стадии `extract` (форматы FromSoft → нейтральное представление) и `cook` (→ форматы PSP), детерминированные, с кэшем по хэшу. Что ещё не сделано:

| Шаг плана | Сейчас |
|---|---|
| Ячейки мира по коллизиям MSB, видимость `draw_groups ∩ display_groups` (§3.4, 2.1) | нет — сцена из явного списка кусков |
| Позиции int16 + UV u16 в [0,2) с `sceGuTexScale/TexOffset` на батч, вершина мира 12 Б (2.5) | нет — float, 24 Б |
| CLUT4 для мира, мипы (2.10) | только CLUT8, без мипов |
| Многослойные материалы: доминирующий слой / запечённая смесь (2.4) | берётся только слот `g_Diffuse` (первый), второй диффуз отбрасывается; долю многослойных ещё не считали |
| Скин-меш: урезанный скелет ≤32 кости, чанки A8/A4, веса u8 с суммой 128 (2.6–2.7) | нет — поза запекается на CPU (M3) |
| Анимации `.panm` 30 Гц int16, root motion отдельно (2.8) | декод одного кадра HKX есть, упаковки нет |
| TAE → `.evt`, PARAM → C-таблицы, коллизия `.pcol`, навмеш, сущности `.pent`, FMG + шрифт с кириллицей, звук VAG/AT3 (2.9–2.18) | нет |
| Упаковка `zone_<id>.pak` + валидатор бюджетов (exit 1 при превышении) (2.16) | нет |
| pytest на синтетике без ассетов FS (2.17) | нет |

Открытые вопросы конвейера из плана (закрываются в M1): семантика групп видимости на m18_01; доля многослойных материалов; ID клипов c0000 для меча и щита; откуда AtkParam игрока берёт точки хитбокса (dummy оружия или персонажа). Их статус — в status notes.
