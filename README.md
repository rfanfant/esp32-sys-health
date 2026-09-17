# esp32-sys-health

Header-only heap / stack / reset-cause health monitor — plus a pull-model OTA
module — for ESP32 boards running the Arduino-ESP32 framework (core 2.x and 3.x). Transport-agnostic: it produces
numbers and reason strings, and the application decides whether to log them,
send them over MQTT, CAN, Telegram, or a serial console.

Used by the Greenhouse_System master and slave nodes and by ESP32-InternetMonitor.

## What it provides

| Function | Purpose |
|---|---|
| `sysHealthInit(tasks, n)` | Register the tasks whose stacks should be watched. Call once in `setup()` after the task handles exist. |
| `sysHealthSample(s)` | Fill a `SysHealthSample`: free/min heap, largest block, fragmentation now and worst-since-boot, PSRAM free, per-task stack headroom, uptime. |
| `sysHealthCheck(s, reason, n, code, taskIdx)` | 0 ok / 1 warn / 2 fault against configurable floors, with an actionable reason string. Warnings are rate-limited. |
| `sysHealthPrepareReboot(code, taskIdx, s)` | Record the fault in RTC slow memory (survives software, WDT and panic resets, no flash wear). The caller then does its own safe-state work and `esp_restart()`. |
| `sysHealthBootInfo(b)` / `sysHealthBootString(b, out, n)` | Decode `esp_reset_reason()` and consume the prior-reboot record. |
| `sysHealthCrashReport(out, n)` | Core-dump summary after a panic, if the build stores core dumps to flash. |
| `sysHealthPsramSteer(threshold)` | Route large allocations to PSRAM. No-op without PSRAM. |
| `sysHealthCanStats(c)` | TWAI/CAN error counters, if `driver/twai.h` is available. |

**Units are bytes everywhere.** On ESP-IDF, `xTaskCreate` stack depth and
`uxTaskGetStackHighWaterMark()` are both in bytes (`StackType_t` is `uint8_t`).
Do not multiply by 4.

## Install

Add the library to `platformio.ini`, pinned to a tag so builds are reproducible:

```ini
lib_deps =
    https://github.com/rfanfant/esp32-sys-health.git#v1.1.0
```

Picking up a fix is a tag bump. Never point `lib_deps` at a branch.

## Configure

Define before including to override the defaults:

```cpp
#define SYSH_HEAP_FLOOR_BYTES   20000    // fault when free internal heap drops below this
#define SYSH_STACK_FLOOR_BYTES  1024     // fault when any watched task's headroom drops below this
#define SYSH_WARN_RATELIMIT_MS  600000   // minimum spacing between warn reports
#define SYSH_MAX_TASKS          4        // size of the stack table
#include <sys_health.h>
```

Warn tier is heap below 1.5x the floor or stack headroom below 2x the floor.

## Minimal use

```cpp
#include <sys_health.h>

TaskHandle_t hLoop, hNet;
SysHealthTask healthTasks[] = {
    { "loop", &hLoop, 8192 },   // configured stack size in BYTES
    { "net",  &hNet,  6144 },
};

void setup() {
    // ... create tasks so the handles are valid ...
    sysHealthInit(healthTasks, sizeof(healthTasks) / sizeof(healthTasks[0]));

    SysHealthBoot b; sysHealthBootInfo(b);
    char line[160]; sysHealthBootString(b, line, sizeof(line));
    Serial.println(line);           // e.g. "reset=software prior=low-heap heap=18212 ..."
}

void healthTick() {                 // call every 30-60 s from any task
    SysHealthSample s; sysHealthSample(s);
    char reason[200]; uint8_t code, taskIdx;
    uint8_t lvl = sysHealthCheck(s, reason, sizeof(reason), code, taskIdx);
    if (lvl == 1) Serial.println(reason);
    if (lvl == 2) {
        Serial.println(reason);
        sysHealthPrepareReboot(code, taskIdx, s);
        // put hardware in a safe state, flush logs, then:
        esp_restart();
    }
}
```

## ota_pull.h — pull-model OTA (v1.1.0)

Second header, same library, same rules (header-only, include from one
translation unit, policy stays in the application). Lifted from
ESP32-InternetMonitor's inline implementation so the greenhouse master and the
monitor share one copy.

| Function | Purpose |
|---|---|
| `otaPullBootCheck(cfg, info)` | Once in `setup()`. Reports whether this image booted `PENDING_VERIFY` (must be validated or rolled back) and whether this boot is a rollback from a version we tried (blacklisted in NVS). |
| `otaPullPending()` / `otaPullMarkBootValid()` / `otaPullRollbackNow()` | The validation contract. The application decides what "this build works" means and calls one of the two. |
| `otaPullFetchManifest(cfg, m, detail, n)` | GET + parse the flat JSON manifest (`version`, `url`, `sha256`, optional `size`), and if `cfg.hmacKey` is set, require and verify `hmac` = HMAC-SHA256 over `version
url
sha256
size`. |
| `otaPullShouldInstall(cfg, m, detail, n)` | Strictly newer than `cfg.runningVersion` and not blacklisted. |
| `otaPullDownloadAndStage(m, progress, detail, n)` | Stream the image into the inactive slot, SHA-256 checked **before** `Update.end()`. On success the new slot is selected; the caller does its safe-state work and `esp_restart()`s. |
| `otaPullCompare(a, b)` / `otaPullIsNewer(a, b)` | Up to four integers from the leading token: `2.5.0`, `v4.3-s9`, `2.5.0 (2026-09-17)` all compare as expected. |

The header defines `verifyRollbackLater()` to return `true` so the Arduino core
leaves a new image `PENDING_VERIFY` until the application confirms it; any reset
before that rolls back in the bootloader. Define `OTA_PULL_NO_ROLLBACK_HOOK` to
opt out.

```cpp
#include <ota_pull.h>
static const OtaPullConfig cfg = { OTA_MANIFEST_URL, OTA_HMAC_KEY, FW_VERSION };

void setup() {
    OtaPullBootInfo b; otaPullBootCheck(cfg, b);
    // later, once the build has proven itself (network up, peers answering):
    //   if (otaPullPending()) otaPullMarkBootValid();
}

void pollOnce() {                       // from a low-priority task
    OtaPullManifest m; char d[160];
    if (otaPullFetchManifest(cfg, m, d, sizeof d) != OTA_PULL_FETCH_OK) return;
    if (!otaPullShouldInstall(cfg, m, d, sizeof d)) return;
    if (!applicationIsIdle()) return;   // policy: yours
    if (otaPullDownloadAndStage(m, nullptr, d, sizeof d)) { /* safe state */ esp_restart(); }
}
```

`tools/ota_publish.py` in each consuming project writes the manifest (and the
`hmac` field when given a key).

## Notes

- Both headers hold module state in `static` variables and an `RTC_NOINIT_ATTR`
  record. Include it from exactly one translation unit per firmware image.
- `sysHealthBootInfo()` consumes the prior-reboot reason, so call it once at boot
  and keep the result if more than one reporter needs it.
- Requires ESP-IDF headers that ship with Arduino-ESP32; no other dependencies.
