# PSP-est: модель стоимости PSP-3000 для прогонов в эмуляторе

**14.10.2026.** Код: `src/perf/` (C, работает внутри EBOOT), `tools/perf/` (Python), `scripts/psp-est.sh`.
Коэффициенты — **только** в `src/perf/psp_cost.cfg`. **Модель не откалибрована** (`calibrated = 0`): замеров с нашей PSP ещё нет, все числа ниже — **[оценка]** с полосой неопределённости ×2–3 (см. «Точность»).

## Зачем

PPSSPP не годится как секундомер PSP (`plan notes` §6.2):
- заливку 3D-треугольников он не считает;
- стоимость вершины у него выдуманная (20 тактов, в коде комментарий «pure guesswork»);
- MIPS-код идёт по 1 такту на инструкцию (VFPU 2, `div.s` 29), без кэшей **[проверено по коду PPSSPP 400a97f: `GPUCommon.cpp`, `MIPSTables.cpp`]**.

PSP-est не пытается эмулировать такты. Он **считает работу кадра** так, как её увидел бы GE, и переводит её в миллисекунды реальной PSP по коэффициентам. Коэффициенты затем подгоняются под бенч с железа.

## Как работает

1. **GE.** Каждый законченный дисплей-лист (и всё, что он вызывает через CALL) после `sceGuSync` проходится команда за командой (`ge_scan.c`). Учитывается состояние GE: тип вершины, матрицы (кости → мир → вид → проекция), вьюпорт, ножницы, Z, смешивание, освещение, текстура и CLUT. Для каждого вызова отрисовки:
   - **стадия вершин:** число обработанных вершин (по индексу, без кэша после трансформации **[версия]**), байты вершин и индексов, веса скиннинга, освещение, сборка треугольников;
   - **стадия пикселей** (`coverage = 1`): вершины трансформируются на CPU, каждый треугольник отсекается по ближней плоскости и по ножницам, отбрасываются задние грани (правило взято из растеризатора PPSSPP). Считаются покрытые пиксели с перерисовкой (пиксели, отброшенные Z-тестом, тоже: GE читает для них Z), Z-чтение и Z-запись, смешивание. Для текстур считаются **байты текселей на пиксель** = bpp × (текселей на пиксель) × билинейный множитель, с потолком в одну строку кэша (16 Б). Этот потолок моделирует пробуксовку кэша текстур 8 КБ на уменьшенных текстурах без мипмапов. Отдельно считаются текстуры в RAM и в VRAM;
   - **мипмапы (с 15.10):** при `TexMode maxmips > 0`, фильтре `*_MIPMAP_*` и `TexLevelMode AUTO` уровень = ⌊0,5·log2(текселей на пиксель) + bias⌋, тексели на пиксель делятся на 4 за уровень; `*_MIPMAP_LINEAR` читает ещё и следующий уровень. RAM/VRAM считается по адресу каждого уровня. Формула LOD — как в PPSSPP **[оценка]**: замеров мипов на GE нет;
   - **время вызова** = `prim + max(вершины, пиксели) + overlap × min(вершины, пиксели)` (стадии GE работают конвейером) + смены текстуры + загрузки CLUT. На лист добавляется `list + cmds × cmd`.
2. **CPU.** Эмулированное время активных отрезков кадра (`sceKernelGetSystemTimeLow`) × `cpu_k`. Отрезки идут от возврата из `sceGuSwapBuffers` до каждого `sceGuSync` и не включают ожидание GE, vblank, собственное время модели и отладочный вывод после sync. Если в игре стоит патч, время разбивается по системам (`perf_begin/perf_end`).
3. **Кадр.** `frame = max(cpu, cpu_до_листа + ge)`. GE исполняет DIRECT-лист, пока CPU его ещё строит: `sceGuDrawArray` двигает stall-адрес **[проверено: `psp-nm libpspgu.a` — `sceGeListUpdateStallAddr` в `sceGuDrawArray.o`]**. FPS квантуется по vsync: 59,94/n.
   - Всё считается для двух частот: той, на которой реально идёт программа, и для 333/166. GE-коэффициенты заданы для шины 166 МГц и масштабируются как 166/шина. Это подтверждено измерением **[MEAS: «на 333/166 всё занимает 2/3»]**.
4. **Вывод.**
   - Оверлей из 2 строк в левом нижнем углу: `PSP-est 30fps 21.2ms cpu 1.1 ge 20.6 w 25.8` (FPS и мс при 333; `w` — худший кадр), треугольники, перерисовка, вызовы отрисовки, `UNCAL`, `CPU<333!`, `TEX>512!`.
   - В лог: строка `PSPEST f=…` на каждый кадр, `PSPEST-WARN`, `PSPEST-MEM`, `PSPEST-INIT`.
   - `tools/perf/report.py` сводит лог: средний FPS, p95, худший кадр, части GE и критерий §4.5 на оценке.

### Длинные прогоны (с 15.10)

- `analyze_every = N` в cfg: листы разбираются только каждый N-й кадр (остальные кадры модель не трогает). Полный маршрут «Прибежища» ~26 тыс. кадров — с `N = 30`.
- `scripts/psp-est.sh asylum <out> <route|hollow|death|boss> [max_frames] [secs] [dump_every]` — EBOOT «Прибежища» (`ash_asylum_est`, `ANALYZE` = N, по умолчанию 30). `EBOOT=build/perf/pbp_ash_asylum_fast_est/EBOOT.PBP` — та же игра с патчем скорости perf-night notes; `OPT=slow|nopipe` — рычаги по отдельности. Запуск через `scripts/emu-slot.sh`, сырой `run.log` удаляется, остаётся `pspest.log` (строки `PSPEST`/`ASY:`/`ash-asy`).

### Подключение без правки игры

`src/perf/CMakeLists.txt` — отдельный проект. Он собирает **неизменённые** исходники `src/game`, `src/viewer` и `src/bench` вместе с `src/perf` и линкует их с `-Wl,--wrap=` на пять вызовов: `sceGuStart`, `sceGuFinish`, `sceGuSync`, `sceGuSwapBuffers`, `sceGuDrawBuffer`. Общие файлы не тронуты.

```bash
scripts/psp-est.sh build                                         # -> build/perf/pbp_ash_{game,viewer,bench}_est/EBOOT.PBP
scripts/psp-est.sh viewer ~/ash-assets/cooked/hall.ashm ~/ash-assets/shots/pspest-viewer "0 1 2 3" 12
SCENE=arena scripts/psp-est.sh game ~/ash-assets/cooked ~/ash-assets/shots/pspest-game 390 240
scripts/psp-est.sh bench ~/ash-assets/shots/pspest-bench         # признаки бенча для калибровки
EXTRA_CFG="cpu_k = 2.5;ge_vert_ns = 120" scripts/psp-est.sh game …  # прогон с другими коэффициентами без пересборки
```

Каждый прогон идёт через `flock ~/ash-assets/.emu.lock`, `nice -n 19` и shm-shim. Конфиг кладётся рядом с EBOOT как `psp_cost.cfg` и читается при старте, поэтому для новых коэффициентов пересборка не нужна.

**Эмулированного времени нужно больше.** Анализ кадра сам тратит эмулированное время (~0,1–0,5 с на кадр сцены). Игра в режиме автопилота детерминирована (ровно 1 шаг на кадр), поэтому это не влияет на картинку, но таймаут `secs` нужен в 5–10 раз больше обычного. 390 кадров игры — это `secs = 240`, около 30 с реального времени.

## Жёсткий режим «как на железе» (память, лимиты)

- **Раздел памяти.** PPSSPP (`MemMap.h`, `System.cpp`) даёт пользователю **24 МБ** (0x08800000–0x0A000000), если в SFO не `MEMSIZE=1`. У нас `MEMSIZE=2` (по умолчанию в pspdev), и для homebrew эмулятор при этом отображает лишние 32 МБ **вне** раздела. При `MEMSIZE=1` раздел расширяется до 0x0C000000 (56 МБ). У реальной PSP-3000 обычный раздел тоже 24 МБ **[SPEC]**. Сколько у расширенного раздела свободно на деле (в сообществе говорят «~52 МБ»), неизвестно **[версия]**: скажет `ASHMEM64` на железе.
- **Проверено прогоном** (`PSPEST-INIT mem`): игра и вьюер в PPSSPP получают раздел 24 МБ (`emu_partition=32MB(24MB user)`), куча newlib при старте около 22 МБ. После загрузки арены свободно 19,1 МБ, после зала 18,7 МБ.
- **Сверка с железом.** `calibrate.py fit` сравнивает `mem.maxfree_kb` бенча в эмуляторе (`tools/perf/bench_emu_results.txt`: 18 969 КБ) и на PSP и пишет разницу в `mem_hw_minus_emu_kb`.
- **`mem_enforce = 1`** при старте выделяет в куче балласт: `mem_hw_minus_emu_kb` плюс 32 МБ, если стоит профиль 32, а эмулятор дал 64-мегабайтный раздел. После этого `malloc` отказывает там же, где отказал бы на PSP. **[проверено]**: при балласте 18 000 КБ вьюер ещё влезает (зал 2,7 МБ).
- **`PSPEST-MEM`** раз в 60 кадров замеряет свободную кучу бинарным поиском `malloc`. Если запас на железе меньше `mem_reserve_kb = 2048` (§4.1), в лог идёт `BELOW_RESERVE` и `PSPEST-WARN free_heap_kb_hw`.
- **Собственный след модели:** около 146 КБ (кэш вершин 96 КБ + буферы). Измерено как разница `mem.maxfree_kb` бенча с моделью и без неё (18 823 против 18 969 КБ).
- **Лимиты и бюджеты** (`PSPEST-WARN`): текстура больше 512 (предел GE), текстура не CLUT4/CLUT8, больше 8 костей на вызов, частота CPU ниже 333. Бюджеты §4.3–4.4: треугольники ≤22 000, вызовы отрисовки ≤250, смены текстуры ≤60, GE ≤30 мс, CPU ≤18,5 мс, кадр >33,3 мс. В лог пишутся первые 5 случаев каждого вида, потом каждый 300-й.

## Коэффициенты и источники

Полный список с пометками — в `src/perf/psp_cost.cfg`. Главные:

| Коэффициент | Значение | Откуда |
|---|---|---|
| пиксель, 16-бит цвет | 2,51 нс | **MEAS:** полноэкранная очистка 0,49 мс при 222/111 (pspautotests `gpu/timing/blittiming`, цитата в `GPUCommon.cpp`) × 2/3 |
| очистка 8888 / 5650 | ×1,41 | MEAS: 0,69 / 0,49 мс |
| байт текселя из RAM | 10,4 нс (× 1,17 конкуренция за шину) | MEAS-derived: спрайт 1:1 16-бит из RAM 34,9 нс/пикс при 222 |
| байт текселя из VRAM | 1,51 нс | MEAS-derived: тот же тест из VRAM 8,3 нс/пикс |
| потолок байт на пиксель | 16 Б | MEAS-derived: широкий спрайт пробуксовывает кэш, 253 нс/пикс ≈ 16 Б × 15,6 нс/Б |
| вершина | 60 нс + 0,75 нс/байт | CODE: 20 тактов PPSSPP («pure guesswork») × 2/3; шина 128 бит × 166 МГц = 2,66 ГБ/с [SPEC], ×2 [оценка] |
| освещение | 30 + 21 нс на источник | CODE (догадка PPSSPP) |
| вес скиннинга | 6 нс | [оценка] |
| треугольник / вызов / лист / команда | 40 / 150 / 360 / 12 нс | [оценка] / [оценка] / CODE 60+60 тактов / CODE 2 такта ×2 |
| Z-чтение / Z-запись / смешивание | 1,25 / 1,25 / 2,5 нс | [оценка]; тест блитов разницы от смешивания не увидел (для 1:1-спрайтов) |
| смена текстуры, загрузка CLUT | 600 нс, 1,5 нс/Б | [оценка], публичных замеров нет |
| перекрытие стадий GE | 0,25 | [оценка] |
| **cpu_k** | **1,8** | [оценка 1,5–2,5]: PPSSPP считает 1 такт на инструкцию; у Allegrex есть промахи кэша 16+16 КБ, задержки load-use и FPU. VFPU: vadd/vmul задержка 5, vdot 7, vdiv.s 17 тактов (pspdev vfpu-docs) |

**Чего в открытых источниках нет вообще** (ресёрч, 25 источников, список ниже): стоимость вершины по форматам (float/s16/s8), наличие кэша вершин, стоимость вызова отрисовки, смены текстуры и CLUT, ускорение от свиззла, штраф промаха кэша текстур в тактах, задержки FPU, промах D-кэша, реальная скорость memcpy, отношение PPSSPP/PSP для CPU. Всё это закрывает только наш бенч.

## Калибровка (когда будет PSP)

1. Бенч `src/bench` на PSP пишет `results.txt` (the psp-day1 design notes).
2. `tools/perf/calibrate.py fit --results <results.txt> --write`:
   - берёт признаки каждого теста бенча из `tools/perf/bench_features.json`. Они сделаны тем же кодом, что считает игру: `scripts/psp-est.sh bench`, 31 тест — `tri.*` 13, `fill.*` 9, `open.*` 2, `skin.*` 7;
   - подгоняет 14 GE-коэффициентов нелинейным МНК по log(времени) (scipy, границы ≥0). Слабая привязка к текущим значениям держит на месте те коэффициенты, которых бенч не видит;
   - `skin.*` идёт в подгонку как `build + wait` — время занятости GE в цикле бенча;
   - `cpu_k` — среднее геометрическое отношений «железо / эмулятор» в CPU-тестах бенча при 333 МГц (`loop` и `pose32` с весом 2, `matmul fpu/vfpu` с весом 1). Эмуляторная база — `tools/perf/bench_emu_results.txt`, чистый бенч без модели;
   - `mem_hw_minus_emu_kb` — разница `mem.maxfree_kb`;
   - печатает по каждому тесту «железо / до / после», ставит `calibrated = 1` и записывает в cfg новые значения с пометкой `[fit дата файл] was …`.
3. Пересобрать (`scripts/psp-est.sh build`) или просто положить новый cfg рядом с EBOOT.
4. **Проверка без железа:** `calibrate.py selftest` делает «железо» из коэффициентов cfg × U(0,4…2,5) с шумом 3% и восстанавливает их. **[проверено]**: ошибка предсказания 24% → 1,2% (максимум 2,7%) на 31 тесте. Вершины, байты, треугольники, смешивание, RAM-текстуры, перекрытие и веса восстанавливаются с точностью ±3–15%. VRAM-текстуры, `prim` и `list` бенч не видит, они остаются как были (см. ниже). Разбор настоящего формата `results.txt` проверен на строках эмуляторного бенча.

**Чего не хватает бенчу для полной калибровки** (это зона владельца `src/bench`, я его не трогал):
- (а) `fill.clut8vram` — текстура из VRAM в режиме, где упор в заливку;
- (б) **уменьшенная текстура:** 128² CLUT8, натянутая на 32×32 пикселя с тайлингом ×8, — именно это тормозит сцену зала;
- (в) много мелких вызовов (1 000 × 6 вершин) — стоимость вызова и смены текстуры;
- (г) 8888-кадр;
- (д) скиннинг с освещением и без.

## Результаты на существующих сценах (эмулятор + модель, при 333/166, `calibrated = 0`)

Команды выше. Логи и json лежат в `~/ash-assets/shots/pspest-*`. «Оптим./песс.» — тот же прогон с коэффициентами, сдвинутыми в обе стороны:
- оптимистичный набор: вершина 30 нс, 0,38 нс/Б, RAM-тексель 5,2 нс/Б, потолок 8 Б, пиксель 1,7 нс, треугольник 20 нс, `cpu_k` 1,3;
- пессимистичный набор: 120 нс, 1,5 нс/Б, 15,6 нс/Б, потолок 32 Б, пиксель 3,77 нс, треугольник 80 нс, `cpu_k` 2,5.

| Сцена | PSP-est FPS (vsync) | Кадр сред / p95 / худший, мс | GE / CPU, мс | Оптим. / песс. FPS | Как собрано (222 МГц) |
|---|---|---|---|---|---|
| Игра, арена (c2232, рыцарь + Демон, автопилот 390 кадров) | **32** (30 FPS в 100% кадров) | 21,2 / 25,3 / 25,8 | 20,6 / 1,1 | 60 / 23 | 24 FPS |
| Игра, яма (c2230, `SCENE=hall`) | **26,5** (30 FPS в 65% кадров) | 28,3 / 40,7 / 41,4 | 27,7 / 1,1 | 50 / 19 | 20 FPS |
| Вьюер, полный зал `hall.ashm` (44 тыс. треуг.), камеры 0/1/3 | **20** | 42–44,5 | ~44 / 0,3 | 60 / 10 (кам. 0) | — |
| Вьюер, камера 2 | **30** | 19,4 | 19,3 / 0,2 | 60 / 20 | — |
| Бенч, ворота 2: 10 тыс. треуг. CLUT4, перерисовка 2× | GE 4,25 мс (зелёный ≤12) | — | — | — | — |
| Бенч, ворота 1: 6 персонажей × 2 048 треуг., A8 | GE 7,9 мс + CPU | — | — | — | — |

**Что видно и что из этого делать** (все числа — [оценка] модели):
- **Игра не ставит 333 МГц.** На PSP homebrew стартует на 222/111, и GE с CPU работают в 1,5 раза медленнее: арена 32 → 24 FPS, яма 26,5 → 20. Исправляется одной строкой (`scePowerSetClockFrequency(333,333,166)`, есть в патче ниже). Без неё в лог идёт `PSPEST-WARN cpu_clock_mhz`.
- **CPU сейчас не узкое место:** 1,1 мс на 333 при 2 персонажах. ИИ и боя ещё нет.
- **GE в арене упирается в вершины.** Из 20,6 мс на стадию вершин приходится 15,5 мс (последовательная сумма), на пиксели 10,3:
  - на кадр уходит **140 тыс. обработанных вершин** — float, 26 Б с индексом, по 3 на треугольник;
  - из 47 тыс. отправленных треугольников 12 тыс. отбрасываются как задние, 13 тыс. срезаются ближней плоскостью. Сфера на весь батч отсекает грубо;
  - рычаги: вершины s16 (24 → 10–12 Б), полосы вместо списков, батчи мельче, чтобы отсечение работало.
- **Пиксели в зале и во вьюере дорогие из-за уменьшения текстур.** Тайлинг DS даёт в среднем **16 текселей на пиксель** (текстуры 128² CLUT8 из RAM, UV до ±25 повторов), мипмапов нет, кэш 8 КБ пробуксовывает.
  - Рычаги: мипмапы в кукере, VRAM для текстур мира, CLUT4.
  - Этот вывод держится на потолке «16 Б на пиксель», и именно его бенч пока не меряет — тест (б) выше.
- **Треугольники выше бюджета.** После отсечения 22–27,5 тыс. треугольников в кадре при бюджете 22 тыс. (`PSPEST-WARN tris`).

## Точность — честно

- **Железа не было ни разу.** Модель не откалибрована. Измерены чужими руками на настоящей PSP только скорости заливки и чтения текстур в 2D-блитах (pspautotests, через комментарии в PPSSPP). Стоимость вершины — догадка PPSSPP. Остальное — мои оценки.
- **Полоса неопределённости ×2–3 в обе стороны.** Арена: от 23 до 60 FPS, яма: от 19 до 50 FPS. Чему из этого верить, станет ясно только после первого `results.txt`. После калибровки на бенче ожидаю ±20–30% на похожих сценах **[версия]**. Это проверяется тем же бенчем на железе: отложить 5 тестов из подгонки и сравнить.
- **Чего модель не видит:**
  - ранний Z-отказ (если GE пропускает текстурирование для отброшенных пикселей, модель пессимистична);
  - реальное поведение кэша текстур (только формула с потолком) и кэш CLUT;
  - конкуренцию CPU и GE за шину сверх постоянного ×1,17;
  - промахи D-кэша и некэшируемый доступ (один `cpu_k` на всё);
  - стоимость системных вызовов: в PPSSPP HLE почти бесплатен, а `sceKernelDcacheWritebackAll`, `sceIo*` и `sceCtrl` на железе стоят денег;
  - аудиопоток и ME, планировщик потоков, недогрузку GE (stall), когда CPU строит лист медленнее, чем GE его ест (частично покрыто формулой `max(cpu, pre + ge)`);
  - morph, bezier и spline (только счёт вершин);
  - смену текстуры внутри CALL-листа: она учитывается, но перекрытие по нескольким листам — нет.
- **На железе** `*_est` EBOOT тоже работает. Тогда `PSPEST` пишет ещё `wall=` (настоящее время кадра) и `gewait=` (ожидание `sceGuSync`). Для честного времени кадра на железе ставить `coverage = 0` и `overlay = 0`: трансформация вершин на CPU стоит миллисекунды.

## Интеграция в игру (патч для интегратора)

> **15.10: для «Прибежища» (`asy_main.c`) актуален `src/perf/asy_fast.patch`** (`option(ASH_PERF)` + секции CPU + быстрый путь отрисовки), см. perf-night notes §5. Патч ниже — для старого EBOOT `ash_game` (`main.c`, M4); 333 МГц в «Прибежище» уже стоят.

Мои файлы — только `src/perf`, `tools/perf`, `scripts/psp-est.sh` и этот документ. `src/game` я не менял. Зачем патч:
- игра начнёт ставить 333 МГц (это нужно в любом случае);
- появятся разбивка CPU по системам (`secs=col:…,anim:…,list:…` в `PSPEST`) и сборка `-DASH_PERF=ON` прямо в основном дереве.

Без `ASH_PERF` макросы пустые, и игра собирается как раньше.

**[проверено]:**
- патч применяется (`git apply --check`) к `main` от 14.10 17:40;
- пропатченная игра собирается с `-DASH_PERF=1`;
- в эмуляторе видно `mhz=333/166` и `secs=anim:0.06,col:0.47,list:0.64`.

Применить: `sed -n '/^--- a\/src\/game\/CMakeLists.txt/,/^```$/p' docs/psp-cost-model.md | sed '$d' | git apply`.

```diff
--- a/src/game/CMakeLists.txt
+++ b/src/game/CMakeLists.txt
@@ -1,8 +1,20 @@
 add_executable(ash_game main.c anim.c chr.c camera.c input.c scene.c pcol.c)
-target_link_libraries(ash_game PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
+target_link_libraries(ash_game PRIVATE psppower pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
 
 create_pbp_file(
   TARGET ash_game
   TITLE "Ash M4"
   OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
 )
+
+# PSP-est (docs/psp-cost-model.md): cmake -DASH_PERF=ON -> the cost model, overlay and PSPEST log in this EBOOT
+option(ASH_PERF "link src/perf (PSP-3000 cost model)" OFF)
+if(ASH_PERF)
+  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
+    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/perf/cfg2h.py ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
+    DEPENDS ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg)
+  target_sources(ash_game PRIVATE ../perf/perf.c ../perf/ge_scan.c ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
+  target_include_directories(ash_game PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
+  target_compile_definitions(ash_game PRIVATE ASH_PERF=1)
+  target_link_options(ash_game PRIVATE "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
+endif()
--- a/src/game/main.c
+++ b/src/game/main.c
@@ -44,6 +44,14 @@
 #include "input.h"
 #include "pcol.h"
 #include "scene.h"
+#ifdef ASH_PERF
+#include "../perf/perf.h"
+#else
+#define perf_begin(s) ((void)0)
+#define perf_end(s) ((void)0)
+#define perf_count_chars(n) ((void)0)
+#endif
+#include <psppower.h>
 
 PSP_MODULE_INFO("Ash M4", 0, 0, 1);
 PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
@@ -472,6 +480,7 @@
     sceCtrlSetSamplingCycle(0);
     sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
     g_tick_res = sceRtcGetTickResolution();
+    scePowerSetClockFrequency(333, 333, 166);   /* the PSP boots homebrew at 222/111: GE and CPU 1.5x slower */
 
     Scene sc;
     if (load_all(&sc) < 0) { sceKernelExitGame(); return 0; }
@@ -613,6 +622,7 @@
                 printf("ash-game: f%u lock broken (distance)\n", sim_frame);
             }
             u64 tc0 = now_ticks();
+            perf_begin(PERF_COL);
             g_col.tests = 0;
             player_tick(&pl, &in, cam.yaw, locked, g_d.pos, DT);
             demon_tick(DT);
@@ -634,6 +644,7 @@
             ci.yaw_in = in.cam_yaw_in;
             ci.pitch_in = in.cam_pitch_in;
             cam_update(&cam, &ccfg, &ci, DT);
+            perf_end(PERF_COL);
             float cus = us_since(tc0);
             col_us = col_us * 0.9f + 0.1f * cus;
             if (cus > col_us_max) col_us_max = cus;
@@ -644,7 +655,9 @@
         }
 
         u64 t0 = now_ticks();
+        perf_begin(PERF_ANIM);
         player_pose(&pl);
+        perf_end(PERF_ANIM);
         float anim_us = us_since(t0);
 
         /* ---- draw */
@@ -679,6 +692,7 @@
         sceGumMatrixMode(GU_MODEL);
         sceGumLoadIdentity();
         int nb = 0;
+        perf_begin(PERF_LIST);
         int tris = scene_draw(&sc, &vp, &nb);
 
         /* knight: model = T(pos) * Ry(yaw); bones = skin matrices; GE lighting on */
@@ -704,6 +718,8 @@
         sceGuDisable(GU_LIGHTING);
         sceGuDisable(GU_FOG);
         if (locked) draw_lock_marker(&vp, g_d.lock_pt);
+        perf_end(PERF_LIST);
+        perf_count_chars(g_d.present ? 2 : 1);
         sceGuFinish();
         float list_ms = us_since(t0) / 1000.0f;
         u64 t1 = now_ticks();
```

API для других систем (`src/perf/perf.h`):
- `perf_begin/perf_end(PERF_SIM|PERF_AI|PERF_ANIM|PERF_COL|PERF_CAM|PERF_LIST|PERF_AUDIO|PERF_OTHER)`;
- `perf_count_chars/bones/coltests/ai(n)`;
- `perf_last()` — оценка прошлого кадра, например для авто-деградации (§4.4);
- ручные хуки `perf_list_started/finished`, `perf_ge_synced`, `perf_frame_end` — если `--wrap` неудобен.

## Поля строки `PSPEST`

- `wall`, `gewait` — реальные мс: время кадра и ожидание sync. В эмуляторе бессмысленны.
- `cpu`, `pre`, `ge`, `frame`, `fps` — оценка на текущей частоте; `*333` — то же при 333/166.
- `emu_cpu` — эмулированные мс CPU.
- `tris`, `tris_in`, `culled`, `clipped` — нарисовано, отправлено, отброшено задних граней, срезано.
- `draws`, `skinned`, `verts` — вызовы отрисовки, из них со скиннингом, вершины всего.
- `vs8`, `vs16`, `vf32`, `vthr` — вершины по формату позиции; `vskin`, `vlit` — со скиннингом и с освещением; `vbytes` — байты вершин и индексов.
- `px` — пиксели; `od` — перерисовка (px / 130 560).
- `texpx`, `texpx_ram` — текстурированные пиксели, из них с текстурой в RAM; `texb_ram`, `texb_vram` — байты текселей.
- `minif` — средние тексели на пиксель.
- `texbind`, `clut`, `clutb`, `bonemats`, `maxw`, `cmds`, `lists`.
- `ns_v`, `ns_p`, `ns_s`, `ns_c` — последовательные суммы по стадиям (вершины, пиксели, состояние, команды), в нс.
- `secs` — CPU по системам; есть только с патчем.

## Источники (ресёрч 14.10, сырой отчёт не включён)

1. PPSSPP `GPU/GPUCommon.cpp` (sha 400a97f, локальный клон `~/opt/ppsspp/src`): `EstimatePerVertexCost`, `EstimateFillCycles` (замеры pspautotests), 2 такта на команду, 60 тактов на лист.
2. PPSSPP `Core/MIPS/MIPSTables.cpp/.h`: 1 такт на инструкцию, VFPU 2, `div.s` 29.
3. PPSSPP `Core/MemMap.h`, `Core/System.cpp`, `Core/PSPLoaders.cpp`: разделы 24/56 МБ, MEMSIZE, отображение для homebrew.
4. PPSSPP `GPU/Software/TransformUnit.cpp`, `Rasterizer.cpp`: правило отбрасывания задних граней.
5. pspautotests `tests/gpu/timing/blittiming` — https://github.com/hrydgard/pspautotests/tree/master/tests/gpu/timing
6. Аппаратные факты PSP — https://en.wikipedia.org/wiki/PlayStation_Portable_hardware
7. Rodrigo Copetti, «PlayStation Portable Architecture» — https://www.copetti.org/writings/consoles/playstation-portable/
8. PC Watch 2003 (спецификация, 2,6 ГБ/с) — https://pc.watch.impress.co.jp/docs/2003/0801/kaigai008.htm
9. pspdev VFPU docs (задержки, пропускная способность) — https://pspdev.github.io/vfpu-docs/
10. OpenStrike PSP_COMBAT_PERFORMANCE — https://raw.githubusercontent.com/pocket-nexus/open-strike/main/docs/PSP_COMBAT_PERFORMANCE.md
11. OpenStrike PSP_CHARACTER_ACCEPTANCE — https://raw.githubusercontent.com/pocket-nexus/open-strike/main/docs/PSP_CHARACTER_ACCEPTANCE.md
12. OpenStrike, репозиторий — https://github.com/pocket-nexus/open-strike
13. PPSSPP: обзор GE — http://www.ppsspp.org/docs/psp-hardware/gpu/ge-overview/
14. PPSSPP: форматы изображений — https://www.ppsspp.org/docs/psp-hardware/gpu/image-formats
15. PPSSPP: обзор Allegrex — http://www.ppsspp.org/docs/psp-hardware/cpu/allegrex-overview/
16. PPSSPP: известные отличия от железа — https://ppsspp.org/docs/development/known-differences
17. Beyond3D, «real PSP performance numbers» — https://forum.beyond3d.com/threads/real-psp-performance-numbers.16088/ (только сниппет, [оценка])
18. Tom's Hardware, «PSP vs PS2 tris/s» — https://forums.tomshardware.com/threads/psp-versus-ps2-triangles-per-second.132085/ (сниппет, [оценка])
19. ps2dev, тред про производительность GE — https://forums.ps2dev.org/viewtopic.php?t=9258 (страница отдала 522, только сниппет)
20. AnandTech, спецификации — https://forums.anandtech.com/threads/oh-good-lordy-playstation-portable-specs.1118489
21. Форумы PPSSPP: JIT против железа — https://forums.ppsspp.org/showthread.php?tid=19465 (качественно, без чисел)
22. Doom PSP — https://doomwiki.org/wiki/Doom_PSP (анекдот)
23. InsideQC, Quake на PSP — https://forums.insideqc.com/viewtopic.php?p=44657 (мнение)
24. Generation Amiga, OpenStrike — https://www.generationamiga.com/2026/07/10/counter-strike-on-psp-openstrike-runs-at-60fps-with-javascript/ (без чисел)
25. `psp-nm`/`psp-objdump` libpspgu.a и libcglue.a (pspdev v20261001): stall-адрес в `sceGuDrawArray`, границы `_sbrk` **[проверено]**.

Отброшено: komstation, «3,5× на 333 МГц» — противоречит соотношению частот 1,5×.
