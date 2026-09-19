# full_demo

A standalone PlatformIO project that exercises **every public API** in
`sys_health.h` from an interactive serial console. Verified on real hardware:
compiles clean for both a generic classic ESP32 (`esp32dev`) and a generic
ESP32-S3 devkit (`esp32s3`), and was flashed to a YD-ESP32-S3-N16R8 (the board
ESP32-InternetMonitor runs on) for the reboot-record round trip below.

## Build

```
cd examples/full_demo
pio run -e esp32dev      # classic ESP32, no board-specific setup needed
pio run -e esp32s3       # generic ESP32-S3 devkit
```

`lib_deps` points at this repo's own `src/` folder (`file://../../src`), so it
always builds against the copy of the library you have checked out — no
network fetch, no tag to keep in sync while you're editing the header. In your
own project, depend on the tagged GitHub URL instead (see the top-level
README's Install section) so your build stays pinned and reproducible.

## Flash + monitor

```
pio run -e esp32s3 -t upload --upload-port COM11
pio device monitor -b 115200 --port COM11
```

(Set `upload_port`/`monitor_port` in `platformio.ini`, or pass
`--upload-port`/`--monitor-port`, for your board's actual port.)

## Commands

Type a letter + Enter in the serial monitor:

| Key | Calls | What you'll see |
|---|---|---|
| `s` | `sysHealthSample()` | free/min heap, largest block, fragmentation now/worst, PSRAM, uptime, and per-task stack headroom for `loop` (Arduino's own loop task) and `churn` (a background task this demo creates) |
| `c` | `sysHealthCheck()` | 0/1/2 verdict against the floors `#define`d at the top of `main.cpp`, with the reason string |
| `f` | `sysHealthPrepareReboot(SYSH_REBOOT_LOW_HEAP, ...)` + `esp_restart()` | a real reboot; the RTC-memory record survives it, so the **next** boot's "Boot info" line reports `prior=low-heap` with the heap/uptime captured the instant before the (simulated) fault |
| `t` | same, with `SYSH_REBOOT_LOW_STACK` and the `churn` task's index | same round trip, `prior=low-stack` |
| `p` | `sysHealthPsramSteer()` | called again at runtime; harmless no-op on a board with no PSRAM |
| `r` | `sysHealthCrashReport()` | only produces a report if the last reset was a real panic (not a `f`/`t` simulated one — those are clean software resets) and the build stores core dumps to flash |
| `k` | `sysHealthCanStats()` | TWAI/CAN error counters. `setup()` installs the TWAI driver in `NO_ACK` mode on GPIO4 (TX) / GPIO5 (RX) specifically so this works with **no CAN transceiver wired up at all** |
| `h` | — | reprint the command list |

Every 5 seconds the loop also runs `sysHealthSample()` + `sysHealthCheck()` on
its own and prints only on WARN/FAULT — the pattern a real application's
watchdog task follows continuously, shown alongside the on-demand commands.

`sysHealthInit()`, `sysHealthBootInfo()`, `sysHealthBootString()`,
`sysHealthResetName()`, and `sysHealthRebootName()` all run once in `setup()`
and their output is on the very first lines after boot — you don't need a
command for those.

## What real output looks like

First boot (power-on):

```
=== esp32-sys-health: full_demo ===
Boot info: reset=power-on softReboots=0
  esp_reset_reason() decoded: power-on
Crash report: none (last reset was not a panic).
TWAI: driver installed and started (NO_ACK mode, GPIO4=TX GPIO5=RX).

Commands: s=sample  c=check  f=simulate LOW_HEAP fault+reboot  t=simulate LOW_STACK fault+reboot  p=PSRAM steer  r=crash report  k=CAN/TWAI stats  h=help
```

`s`, `c`, `k`:

```
sample: heap free=355484 min=339460 largest=303092 frag now=15% worst=15% psram=0 uptime=14s
  task 'loop': 5996 B headroom
  task 'churn': 3368 B headroom
check: level=0 (0=ok 1=warn 2=fault) reason=""
TWAI: state=1 tx_err=0 rx_err=0 rx_missed=0 bus_err=0 arb_lost=0 tx_failed=0
```

`f` (LOW_HEAP fault) — note the reboot in the middle and the "Boot info" line
that follows it:

```
Simulating a LOW_HEAP fault: sysHealthPrepareReboot() + esp_restart() ...
Watch the 'Boot info' line after reboot report this as the prior reason.
ESP-ROM:esp32s3-20210327
...
=== esp32-sys-health: full_demo ===
Boot info: reset=software prior=low-heap heap=355484 stack=0 uptime=28s softReboots=1
  esp_reset_reason() decoded: software
  prior soft-reboot reason: low-heap
```

`t` (LOW_STACK fault) right after, showing `softRebootCount` accumulating
across resets:

```
Boot info: reset=software prior=low-stack heap=355484 stack=3368 uptime=17s softReboots=2
  esp_reset_reason() decoded: software
  prior soft-reboot reason: low-stack
```
