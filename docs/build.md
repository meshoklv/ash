# Сборка, эмулятор, обходы окружения

Факты на 14.10.2026 **[проверено по README, `scripts/` и `~/opt`]**. Англоязычный оригинал с теми же командами — корневой `README.md`.

## Среда (без sudo)

В контейнере **нет системных gcc/g++/cmake/make**. Всё ставится в `~/opt` (шаг A0 плана решён запасным путём — без Docker, см. decisions notes).

| Что | Где | Размер |
|---|---|---|
| pspdev **v20261001** (psp-gcc 15.2.0), релиз `pspdev-ubuntu-latest-x86_64.tar.gz` | `~/opt/pspdev` | 454 МБ |
| cmake / ninja / make / gcc хоста (conda-forge через `~/opt/bin/micromamba`) | `~/opt/buildtools` | 1,4 ГБ |
| PPSSPPHeadless (software GPU, без SDL), master `400a97f` | `~/opt/ppsspp/PPSSPPHeadless` (+ `src/`, логи сборки) | бинарь 40 МБ |
| venv конвейера, Python 3.13 | `tools/.venv` | 289 МБ |

`scripts/env.sh` выставляет `PSPDEV`, `ASH_BUILDTOOLS`, `PPSSPP_HEADLESS` и `PATH`. Подключать: `. scripts/env.sh`.

### Обход: libmpc / libmpfr для pspdev

`cc1`/`cc1plus`/`lto1` из pspdev требуют `libmpc.so.3` и `libmpfr.so.6`, которых в контейнере нет. Они скопированы в `~/opt/pspdev/hostlib/`, а бинарям прописан `RUNPATH=$ORIGIN/../../../../hostlib` через `patchelf`. **При переустановке pspdev повторить.**

## Сборка

```bash
cd ~/ash && . scripts/env.sh
psp-cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nice -n 19 cmake --build build
# -> build/src/hello/EBOOT.PBP  ("Ash M0 Hello")
# -> build/src/viewer/EBOOT.PBP ("Ash Viewer")
```

- Корневой `CMakeLists.txt` падает, если конфигурировать не через `psp-cmake`.
- Флаги: C99, `-O2 -G0 -Wall`. `PARAM.SFO` получает `MEMSIZE=2` по умолчанию (pspdev ≥ v20260801) — расширенная память на 2000/3000 включена.
- EBOOT — обычный (нешифрованный) ELF → нужен CFW. PRX-сборка (`BUILD_PRX`) для PSPLink — позже.
- Флаги плана `MEM_PROFILE=32|64`, `DEBUG` ещё не заведены.

## Запуск в эмуляторе (headless)

```bash
scripts/run-headless.sh build/src/hello/EBOOT.PBP 5 [out_dir]
# -> tests/out/hello-<ts>/screen.png   последний показанный кадр 480×272
#    tests/out/hello-<ts>/run.log      printf программы + лог эмулятора + "Frame stats"
```

Внутри: `PPSSPPHeadless --graphics=software --root=<папка EBOOT> --timeout-emulated=N --timeout-wall=6N+30 --frame-stats --screenshot-save=… -l` под `nice -n 10`. Затем проверка на чёрный кадр (чистый Python): FAIL, если не-чёрных пикселей <5% или цветов ≤4. Файлы рядом с EBOOT доступны программе как `host0:`/cwd.

Сцена DS по камерам:

```bash
scripts/shoot-viewer.sh <scene.ashm> <out_dir> ["0 1 2 3"] [секунды=4]
```

Кладёт `EBOOT.PBP` вьюера и `scene.ashm` в `build/stage-viewer/`, для каждой камеры пишет `camera.txt` и делает прогон → `<out_dir>/cam<N>/screen.png`. Скрины — в `~/ash-assets/shots`, не в репо.

**FPS эмулятора ≠ FPS железа.** `Frame stats` считает флипы за эмулированную секунду; software-GPU и JIT не потактовые, заливка 3D-треугольников не учитывается, кэши CPU не эмулируются. Производительность — только на PSP (plan notes §6.2).

Не использовать `--debugger=<port>` — открывает сетевой порт.

## Пересборка PPSSPPHeadless

Тяжело (CPU, ~1,5–2,5 ГБ диска **[оценка плана]**): перед запуском `free -h` и `df -h .`.

```bash
B=~/opt/buildtools; export PATH=$B/bin:$PATH CC=$B/bin/x86_64-conda-linux-gnu-gcc CXX=$B/bin/x86_64-conda-linux-gnu-g++
cd ~/opt/ppsspp
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/hrydgard/ppsspp.git src
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON -DHEADLESS_CROSS=ON \
  -DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DVULKAN=OFF -DUSING_X11_VULKAN=OFF -DUSE_WAYLAND_WSI=OFF \
  -DCMAKE_PREFIX_PATH=$B -DUSE_SYSTEM_LIBPNG=OFF -DUSE_SYSTEM_ZSTD=OFF
cp $B/include/GL/{glu.h,gl.h,glext.h} src/ext/glew/GL/ && mkdir -p src/ext/glew/KHR && cp $B/include/KHR/khrplatform.h src/ext/glew/KHR/
nice -n 19 ionice -c3 cmake --build build --target PPSSPPHeadless -j4
x86_64-conda-linux-gnu-objcopy --remove-section .note.gnu.property build/PPSSPPHeadless PPSSPPHeadless
```

Обходы:
- **glew** не находит `GL/glu.h`/`GL/gl.h` в conda-префиксе → копия рядом с `glew.h`. GL-заголовки из conda: `libgl-devel libegl-devel libglu xorg-libx11 xorg-xorgproto`.
- **ISA-note v3.** CPU сборочной машины — Xeon E5-2695 v2 (x86-64-v2, без AVX2), а бинарь получает пометку «ISA needed: v3», и `ld.so` отказывается его запускать. Пометка снимается `objcopy --remove-section .note.gnu.property`. Почему она появляется — **[версия]**: AVX2-файлы в составе; не проверено. Hello работает.
- `HEADLESS_CROSS=ON` = без SDL; работает только `--graphics=software` (тестам это и нужно).
- В релизе v1.20.4 нет `--screenshot-save` и `--frame-stats` — нужен master **[проверено планом]**.
- Для детерминизма реплеев (будущие тесты) sha эмулятора закреплён: `400a97f`. Смена sha = перегенерация эталонов отдельным коммитом.

## На реальной PSP

1. PSP-3000 с CFW (ARK-4 / PRO-C / LME на 6.60/6.61).
2. `EBOOT.PBP` → `ms0:/PSP/GAME/ASH/EBOOT.PBP` (для вьюера рядом `scene.ashm` и, опционально, `camera.txt`; вьюер также ищет в `ms0:/ash/`).
3. XMB → Игра → Memory Stick → «Ash …». START — выход.

Порядок первого дня с железом (модель, свободная память, калибровка, первый честный FPS) — psp-day1 notes . Перенос на PSP — копированием на карту памяти (`ms0:`).

## Не сделано из инфраструктуры плана (M0)

`tools/test.sh` (пакетный прогон), эталонные скриншоты с `--max-mse`, pre-commit против ассетов, `diskguard.sh`, `push-mac.sh`/`push-psp.sh`, `MEM_PROFILE`, кириллица в оверлее, вывод `TEST:{json}`/`PERF:{json}`.
