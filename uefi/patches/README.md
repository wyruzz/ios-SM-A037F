# Патчи для `xnu_bootshim_SM-A037F`

## Целевая ветка

```
branch: audit/boot-image-inspector
commit: d0f910b3b3f84056d9e9fa4a337700d6225fc84c
```

Проверено: `git apply --check` проходит на `d0f910b`, на `main` (77ed45e) падает
(`error: patch failed: shim.c:163`). Для `audit/fdt-parser-hardening` не
предназначены.

## Порядок применения

```sh
git checkout audit/boot-image-inspector
git reset --hard d0f910b
git apply ../patches/0001-shim-wdt-el3-entry.patch
git apply ../patches/0002-build-script-checks.patch
git apply ../patches/0003-canary-entry.patch
git apply ../patches/0004-no-uart-option.patch
```

| # | Файлы | Что делает |
|---|---|---|
| 0001 | `start.S`, `shim.c` | выключает MTK WDT первой инструкцией; полный спуск EL3→EL2→EL1; `shim_el1(x0)` вместо `x2` |
| 0002 | `build.sh`, `build-macos.sh` | чинит проверку магии (`b'ARM\\x64'` → `b'ARMd'`), делает `build-macos.sh` исполняемым |
| 0003 | `start.S`, оба build-скрипта | режим `EXTRA_CFLAGS=-DCANARY`: WDT off и вечный `wfe`, без EL-drop/стека/C/UART |
| 0004 | `shim.c` | `EXTRA_CFLAGS=-DSHIM_NO_UART`: полностью отключает обращения к UART |

`pack_boot_v2.py` в патчи не входит — это отдельный файл, он заменяет версию
из ветки целиком.

## Сборки

```sh
export CROSS_COMPILE=aarch64-linux-gnu-        # Linux
# export CROSS_COMPILE=aarch64-elf-            # macOS + MacPorts binutils

# 1) канарейка: только WDT off + wfe
EXTRA_CFLAGS=-DCANARY ./build.sh

# 2) полная диагностика без UART
EXTRA_CFLAGS=-DSHIM_NO_UART ./build.sh

# 3) обычная диагностика
./build.sh
```

Упаковка (обязательно со стоком, иначе пустой cmdline):

```sh
python3 pack_boot_v2.py --kernel out/bootshim/Image --stock stock_boot.img --output boot.img
fastboot flash boot boot.img
fastboot reboot
```

## Как читать результат

| Сборка | Поведение | Вывод |
|---|---|---|
| canary | перестал ребутиться, висит на логотипе/чёрный | образ исполняется: LK, cmdline, gzip и подпись в порядке |
| canary | ребутится как раньше | образ не исполняется: cmdline/gzip/AVB/KG, а не код шима |
| no-uart | висит | сброс давал UART (bus hang на выключенном блоке) |
| no-uart | ребутится | дело не в UART; следующий подозреваемый — DTB по мусорному указателю |
| normal | висит, UART молчит | всё работает, вывод просто никуда не идёт |

## Эталонные размеры (aarch64-elf, macOS)

```
normal  48672 bytes
canary  48672 bytes
no-uart 36384 bytes
```

Размеры зависят от тулчейна; авторитетный отпечаток — `sha256` в `out/build.json`.
