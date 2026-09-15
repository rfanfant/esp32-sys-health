#pragma once
// ─── sys_health.h — ESP32 heap / stack / reset-cause health monitor ─────────
//
// Header-only, transport-agnostic. Depends only on Arduino + ESP-IDF. Intended
// to be shared verbatim between projects (greenhouse master/slave, InternetMonitor).
//
// Provides:
//   * sysHealthSample()   — free/min heap, largest block, fragmentation (now +
//                           worst-since-boot ratchet), PSRAM free, per-task stack
//                           headroom, uptime.
//   * sysHealthCheck()    — 0 ok / 1 warn / 2 fault against configurable floors,
//                           with an actionable reason string. Warn is rate-limited.
//   * sysHealthPrepareReboot() — records the fault in RTC slow memory (survives
//                           software/WDT/panic resets, NO flash wear) so the next
//                           boot can report why it happened. Caller then does its
//                           own safe-state work and esp_restart().
//   * sysHealthBootInfo() — esp_reset_reason() decode + prior-reboot record.
//   * sysHealthCrashReport() — core-dump summary after a panic (if enabled).
//   * sysHealthPsramSteer() — route large mallocs to PSRAM (fragmentation relief).
//   * sysHealthCanStats() — TWAI error counters (if driver/twai.h is present).
//
// UNITS: everything is BYTES. On ESP-IDF, xTaskCreate's stack depth and
// uxTaskGetStackHighWaterMark() are both in bytes, not words (StackType_t is
// uint8_t). Do not multiply by 4.
//
// Configuration (define before including to override):
//   SYSH_HEAP_FLOOR_BYTES    reboot floor for free internal heap   (default 20000)
//   SYSH_STACK_FLOOR_BYTES   reboot floor for any task's headroom   (default 1024)
//   SYSH_WARN_RATELIMIT_MS   min spacing between warn reports       (default 600000)
//   SYSH_MAX_TASKS           size of the stack table                (default 4)
// Warn tier is heap < 1.5×floor or stack headroom < 2×floor.
//
#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if __has_include(<esp_core_dump.h>)
#include <esp_core_dump.h>
#define SYSH_HAS_COREDUMP_H 1
#endif
#if __has_include("driver/twai.h")
#include "driver/twai.h"
#define SYSH_HAS_TWAI 1
#endif

#ifndef SYSH_HEAP_FLOOR_BYTES
#define SYSH_HEAP_FLOOR_BYTES   20000
#endif
#ifndef SYSH_STACK_FLOOR_BYTES
#define SYSH_STACK_FLOOR_BYTES  1024
#endif
#ifndef SYSH_WARN_RATELIMIT_MS
#define SYSH_WARN_RATELIMIT_MS  600000UL
#endif
#ifndef SYSH_MAX_TASKS
#define SYSH_MAX_TASKS          4
#endif

// ── Reboot reason codes (fit in a byte; sent over CAN by the slave) ─────────
enum SysHealthReboot : uint8_t {
    SYSH_REBOOT_NONE      = 0,   // no record (power-on, or cleanly consumed)
    SYSH_REBOOT_LOW_HEAP  = 1,
    SYSH_REBOOT_LOW_STACK = 2,
    SYSH_REBOOT_COMMAND   = 3,   // operator-requested restart
    SYSH_REBOOT_OTA       = 4,
    SYSH_REBOOT_OTHER     = 5,
};

static inline const char* sysHealthRebootName(uint8_t c) {
    switch (c) {
        case SYSH_REBOOT_LOW_HEAP:  return "low-heap";
        case SYSH_REBOOT_LOW_STACK: return "low-stack";
        case SYSH_REBOOT_COMMAND:   return "command";
        case SYSH_REBOOT_OTA:       return "ota";
        case SYSH_REBOOT_OTHER:     return "other";
        default:                    return "none";
    }
}

static inline const char* sysHealthResetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        default:                return "unknown";
    }
}

// ── Stack table ──────────────────────────────────────────────────────────────
struct SysHealthTask {
    const char*   name;             // e.g. "loop", "oled"
    TaskHandle_t* handle;           // filled in by the app once the task exists
    uint32_t      configuredBytes;  // what the task was created with (BYTES)
};

// ── Sample ───────────────────────────────────────────────────────────────────
struct SysHealthSample {
    uint32_t freeHeap;          // internal heap free now
    uint32_t minFreeHeap;       // lowest ever since boot
    uint32_t largestBlock;      // largest single allocatable block
    uint8_t  fragNow;           // 0..100, 100 = perfectly fragmented, 0 = one block
    uint8_t  fragWorst;         // max fragNow seen since boot (ratchet)
    uint32_t psramFree;         // 0 if no PSRAM
    uint32_t uptimeSec;
    uint8_t  nTasks;
    uint32_t stackFree[SYSH_MAX_TASKS];   // headroom (bytes) per table entry
};

// ── RTC-memory fault record (survives everything but power loss) ────────────
struct SysHealthRtcRecord {
    uint32_t magic;
    uint8_t  reason;          // SysHealthReboot
    uint8_t  taskIdx;         // for LOW_STACK
    uint16_t softRebootCount; // count of our own esp_restart()s since power-on
    uint32_t heapAtFault;
    uint32_t stackAtFault;
    uint32_t uptimeAtFault;
    uint32_t crc;
};
#define SYSH_RTC_MAGIC 0x48454C54UL  // "HELT"

static RTC_NOINIT_ATTR SysHealthRtcRecord _syshRtc;

static inline uint32_t _syshCrc(const SysHealthRtcRecord& r) {
    // tiny FNV-1a over everything except crc
    uint32_t h = 2166136261UL;
    const uint8_t* p = (const uint8_t*)&r;
    for (size_t i = 0; i < offsetof(SysHealthRtcRecord, crc); i++) { h ^= p[i]; h *= 16777619UL; }
    return h;
}
static inline bool _syshRtcValid() { return _syshRtc.magic == SYSH_RTC_MAGIC && _syshRtc.crc == _syshCrc(_syshRtc); }

// ── Module state ─────────────────────────────────────────────────────────────
static const SysHealthTask* _syshTasks  = nullptr;
static uint8_t              _syshNTasks = 0;
static uint8_t              _syshFragWorst = 0;
static uint32_t             _syshLastWarnMs = 0;
static uint32_t             _syshBootMs = 0;

static inline void sysHealthInit(const SysHealthTask* tasks, uint8_t n) {
    _syshTasks  = tasks;
    _syshNTasks = (n > SYSH_MAX_TASKS) ? SYSH_MAX_TASKS : n;
    _syshBootMs = millis();
    if (!_syshRtcValid()) {                       // first power-on: initialise the record
        memset(&_syshRtc, 0, sizeof(_syshRtc));
        _syshRtc.magic = SYSH_RTC_MAGIC;
        _syshRtc.crc   = _syshCrc(_syshRtc);
    }
}

// Route allocations >= threshold bytes to PSRAM (no-op without PSRAM). Call
// early in setup(), before the big buffers (MQTT, JSON) are allocated.
static inline void sysHealthPsramSteer(size_t thresholdBytes) {
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
    if (psramFound()) heap_caps_malloc_extmem_enable(thresholdBytes);
#else
    (void)thresholdBytes;
#endif
}

static inline uint8_t _syshFragPct(uint32_t freeHeap, uint32_t largest) {
    if (freeHeap == 0) return 0;
    uint32_t pct = 100UL - (uint32_t)((uint64_t)largest * 100ULL / freeHeap);
    return (uint8_t)(pct > 100 ? 100 : pct);
}

static inline void sysHealthSample(SysHealthSample& s) {
    s.freeHeap     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.minFreeHeap  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.fragNow      = _syshFragPct(s.freeHeap, s.largestBlock);
    if (s.fragNow > _syshFragWorst) _syshFragWorst = s.fragNow;
    s.fragWorst    = _syshFragWorst;
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
    s.psramFree    = psramFound() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
#else
    s.psramFree    = 0;
#endif
    s.uptimeSec    = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s.nTasks       = _syshNTasks;
    for (uint8_t i = 0; i < _syshNTasks; i++) {
        TaskHandle_t h = _syshTasks[i].handle ? *_syshTasks[i].handle : nullptr;
        s.stackFree[i] = h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0xFFFFFFFFUL;
    }
}

// Returns 0 = ok, 1 = warn (rate-limited: returns 0 again inside the window),
// 2 = fault (caller should sysHealthPrepareReboot + safe state + esp_restart).
// reason: human-readable, actionable. code/taskIdx: for the RTC record.
static inline uint8_t sysHealthCheck(const SysHealthSample& s, char* reason, size_t n,
                                     uint8_t& code, uint8_t& taskIdx) {
    code = SYSH_REBOOT_NONE; taskIdx = 0xFF;
    // FAULT tier
    if (s.freeHeap < SYSH_HEAP_FLOOR_BYTES) {
        code = SYSH_REBOOT_LOW_HEAP;
        snprintf(reason, n, "LOW HEAP %lu B (floor %u, min-since-boot %lu, largest %lu, frag %u%%)",
                 (unsigned long)s.freeHeap, (unsigned)SYSH_HEAP_FLOOR_BYTES,
                 (unsigned long)s.minFreeHeap, (unsigned long)s.largestBlock, s.fragNow);
        return 2;
    }
    for (uint8_t i = 0; i < s.nTasks; i++) {
        if (s.stackFree[i] == 0xFFFFFFFFUL) continue;
        if (s.stackFree[i] < SYSH_STACK_FLOOR_BYTES) {
            code = SYSH_REBOOT_LOW_STACK; taskIdx = i;
            uint32_t cfg  = _syshTasks[i].configuredBytes;
            uint32_t peak = (cfg > s.stackFree[i]) ? cfg - s.stackFree[i] : cfg;
            uint32_t rec  = ((peak + 2048 + 3) / 4) * 4;
            snprintf(reason, n, "LOW STACK task '%s': %lu B headroom, peak %lu of %lu B. "
                     "ACTION: set stack to >= %lu B",
                     _syshTasks[i].name, (unsigned long)s.stackFree[i],
                     (unsigned long)peak, (unsigned long)cfg, (unsigned long)rec);
            return 2;
        }
    }
    // WARN tier (rate-limited)
    bool warn = (s.freeHeap < (SYSH_HEAP_FLOOR_BYTES * 3) / 2);
    int  warnTask = -1;
    for (uint8_t i = 0; i < s.nTasks && !warn; i++)
        if (s.stackFree[i] != 0xFFFFFFFFUL && s.stackFree[i] < SYSH_STACK_FLOOR_BYTES * 2) { warn = true; warnTask = i; }
    if (!warn) { reason[0] = '\0'; return 0; }
    if (_syshLastWarnMs != 0 && millis() - _syshLastWarnMs < SYSH_WARN_RATELIMIT_MS) { reason[0] = '\0'; return 0; }
    _syshLastWarnMs = millis();
    if (warnTask >= 0)
        snprintf(reason, n, "WARN stack '%s' headroom %lu B", _syshTasks[warnTask].name, (unsigned long)s.stackFree[warnTask]);
    else
        snprintf(reason, n, "WARN heap %lu B (floor %u)", (unsigned long)s.freeHeap, (unsigned)SYSH_HEAP_FLOOR_BYTES);
    return 1;
}

// Record the fault before the caller reboots. Zero flash writes.
static inline void sysHealthPrepareReboot(uint8_t code, uint8_t taskIdx, const SysHealthSample& s) {
    if (!_syshRtcValid()) { memset(&_syshRtc, 0, sizeof(_syshRtc)); _syshRtc.magic = SYSH_RTC_MAGIC; }
    _syshRtc.reason        = code;
    _syshRtc.taskIdx       = taskIdx;
    _syshRtc.softRebootCount++;
    _syshRtc.heapAtFault   = s.freeHeap;
    _syshRtc.stackAtFault  = (taskIdx < s.nTasks) ? s.stackFree[taskIdx] : 0;
    _syshRtc.uptimeAtFault = s.uptimeSec;
    _syshRtc.crc           = _syshCrc(_syshRtc);
}

// Boot-time info. Consumes the prior-reboot record (reason cleared, counter kept).
struct SysHealthBoot {
    esp_reset_reason_t resetReason;
    uint8_t  priorReason;        // SysHealthReboot from the last soft reboot, or NONE
    uint8_t  priorTaskIdx;
    uint16_t softRebootCount;
    uint32_t heapAtFault, stackAtFault, uptimeAtFault;
};
static inline void sysHealthBootInfo(SysHealthBoot& b) {
    memset(&b, 0, sizeof(b));
    b.resetReason = esp_reset_reason();
    if (_syshRtcValid()) {
        b.priorReason     = _syshRtc.reason;
        b.priorTaskIdx    = _syshRtc.taskIdx;
        b.softRebootCount = _syshRtc.softRebootCount;
        b.heapAtFault     = _syshRtc.heapAtFault;
        b.stackAtFault    = _syshRtc.stackAtFault;
        b.uptimeAtFault   = _syshRtc.uptimeAtFault;
        _syshRtc.reason   = SYSH_REBOOT_NONE;         // consume
        _syshRtc.crc      = _syshCrc(_syshRtc);
    }
}
static inline void sysHealthBootString(const SysHealthBoot& b, char* out, size_t n) {
    if (b.priorReason != SYSH_REBOOT_NONE)
        snprintf(out, n, "reset=%s prior=%s heap=%lu stack=%lu uptime=%lus softReboots=%u",
                 sysHealthResetName(b.resetReason), sysHealthRebootName(b.priorReason),
                 (unsigned long)b.heapAtFault, (unsigned long)b.stackAtFault,
                 (unsigned long)b.uptimeAtFault, (unsigned)b.softRebootCount);
    else
        snprintf(out, n, "reset=%s softReboots=%u", sysHealthResetName(b.resetReason), (unsigned)b.softRebootCount);
}

// After a panic: core-dump summary (task, PC, cause, backtrace) if the build
// stores core dumps to flash. Erases the dump once read. Returns false if none.
static inline bool sysHealthCrashReport(char* out, size_t n) {
    out[0] = '\0';
    if (esp_reset_reason() != ESP_RST_PANIC) return false;
#if defined(SYSH_HAS_COREDUMP_H) && defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    if (esp_core_dump_image_check() != ESP_OK) { snprintf(out, n, "panic, no core dump in flash"); return true; }
    esp_core_dump_summary_t* sum = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!sum) { snprintf(out, n, "panic, core dump present (no RAM to read it)"); return true; }
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        int w = snprintf(out, n, "panic task=%s pc=0x%08lx cause=%lu bt=",
                         sum->exc_task, (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.exc_cause);
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 8 && w > 0 && (size_t)w < n; i++)
            w += snprintf(out + w, n - w, "%s0x%08lx", i ? "," : "", (unsigned long)sum->exc_bt_info.bt[i]);
        esp_core_dump_image_erase();
    } else {
        snprintf(out, n, "panic, core dump unreadable");
    }
    free(sum);
    return true;
#else
    snprintf(out, n, "panic (core dump to flash not enabled in this build)");
    return true;
#endif
}

// TWAI/CAN health counters. Returns false if the driver is not installed.
struct SysHealthCan {
    uint8_t  txErr, rxErr;
    uint32_t rxMissed, busErr, arbLost, txFailed;
    uint8_t  state;       // twai_state_t: 0 stopped 1 running 2 bus-off 3 recovering
};
static inline bool sysHealthCanStats(SysHealthCan& c) {
#ifdef SYSH_HAS_TWAI
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return false;
    c.txErr    = (uint8_t)st.tx_error_counter;
    c.rxErr    = (uint8_t)st.rx_error_counter;
    c.rxMissed = st.rx_missed_count;
    c.busErr   = st.bus_error_count;
    c.arbLost  = st.arb_lost_count;
    c.txFailed = st.tx_failed_count;
    c.state    = (uint8_t)st.state;
    return true;
#else
    (void)c; return false;
#endif
}
