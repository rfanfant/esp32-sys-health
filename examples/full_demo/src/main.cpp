// ─── full_demo — exercises every public API in sys_health.h ────────────────
//
// Interactive serial console (115200 baud). Type a letter + Enter:
//   s - sysHealthSample()      : print free/min heap, largest block, frag,
//                                PSRAM, uptime, per-task stack headroom
//   c - sysHealthCheck()       : run the ok/warn/fault check against the
//                                floors configured below and print the verdict
//   f - simulate a LOW_HEAP fault  -> sysHealthPrepareReboot() + esp_restart()
//   t - simulate a LOW_STACK fault -> sysHealthPrepareReboot() + esp_restart()
//         Both 'f' and 't' prove the RTC-memory fault record survives the
//         reset: watch the "Boot info" line right after reboot report
//         exactly what you just simulated, with the heap/stack/uptime
//         figures captured at the moment of the (fake) fault.
//   p - sysHealthPsramSteer()  : call it again (idempotent; harmless no-op
//                                on boards without PSRAM)
//   r - sysHealthCrashReport() : core-dump summary if the LAST reset was a
//                                real panic and core dumps are enabled
//   k - sysHealthCanStats()    : TWAI/CAN error counters (driver is started
//                                in NO_ACK mode in setup() so this works with
//                                no CAN transceiver wired up at all)
//   h - reprint this help
//
// Every 5 s the loop also runs sysHealthSample()+sysHealthCheck() itself and
// prints only on WARN/FAULT, mirroring how a real application's watchdog task
// would use the library continuously rather than on demand.
//
// A background "churn" task allocates and frees a handful of variable-sized
// heap blocks every couple of seconds purely so the free-heap/fragmentation
// numbers move during the demo. Its stack, and the Arduino loop() task's own
// stack, are both registered with the monitor so 's' shows real per-task
// headroom for two different tasks, not just one.

#define SYSH_HEAP_FLOOR_BYTES   20000
#define SYSH_STACK_FLOOR_BYTES  1024
#define SYSH_WARN_RATELIMIT_MS  10000   // short on purpose: this is a demo, not production
#define SYSH_MAX_TASKS          2
#include <sys_health.h>

#if __has_include("driver/twai.h")
#include "driver/twai.h"
#define DEMO_HAS_TWAI 1
#endif

// ── Task table ───────────────────────────────────────────────────────────
// "loop" is Arduino's own loop() task (named "loopTask" by the core; its
// handle already exists by the time setup() runs). "churn" is the demo task
// created below. Stack sizes here are what each task was actually created
// with, in BYTES -- they must match reality for stack headroom to mean
// anything (see the library README's UNITS note).
TaskHandle_t hLoopTask = nullptr;
TaskHandle_t hChurn    = nullptr;

#define CHURN_STACK_BYTES 4096

SysHealthTask healthTasks[] = {
    { "loop",  &hLoopTask, 8192 },              // Arduino core's default loop-task stack
    { "churn", &hChurn,    CHURN_STACK_BYTES },
};

SysHealthBoot g_boot;

#if DEMO_HAS_TWAI
static bool g_twaiReady = false;

// Installed in NO_ACK mode so it runs on a bare devkit with nothing wired to
// the TX/RX pins: no transceiver, no second node, no acknowledgement needed.
// That is enough to make sysHealthCanStats() return real counters -- this
// demo never transmits a frame, it only reads driver status.
static void setupTwaiDemo() {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_4, GPIO_NUM_5, TWAI_MODE_NO_ACK);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
        g_twaiReady = true;
        Serial.println("TWAI: driver installed and started (NO_ACK mode, GPIO4=TX GPIO5=RX).");
    } else {
        Serial.println("TWAI: driver install/start failed.");
    }
}
#endif

static void printSample(const SysHealthSample& s) {
    Serial.printf("sample: heap free=%lu min=%lu largest=%lu frag now=%u%% worst=%u%% psram=%lu uptime=%lus\n",
                  (unsigned long)s.freeHeap, (unsigned long)s.minFreeHeap, (unsigned long)s.largestBlock,
                  s.fragNow, s.fragWorst, (unsigned long)s.psramFree, (unsigned long)s.uptimeSec);
    for (uint8_t i = 0; i < s.nTasks; i++) {
        const char* name = healthTasks[i].name;
        if (s.stackFree[i] == 0xFFFFFFFFUL) {
            Serial.printf("  task '%s': handle not set\n", name);
        } else {
            Serial.printf("  task '%s': %lu B headroom\n", name, (unsigned long)s.stackFree[i]);
        }
    }
}

static void printHelp() {
    Serial.println();
    Serial.println("Commands: s=sample  c=check  f=simulate LOW_HEAP fault+reboot  "
                    "t=simulate LOW_STACK fault+reboot  p=PSRAM steer  r=crash report  "
                    "k=CAN/TWAI stats  h=help");
    Serial.println();
}

// Allocates and frees a handful of variable-sized blocks so free heap and
// fragmentation actually move while you watch 's' output during the demo.
static void churnTask(void*) {
    for (;;) {
        void* blocks[6] = { nullptr };
        for (int i = 0; i < 6; i++) {
            blocks[i] = malloc(200 + (esp_random() % 3800));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        for (int i = 5; i >= 0; i--) {
            free(blocks[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== esp32-sys-health: full_demo ===");

    // sysHealthPsramSteer(): route large allocations to PSRAM. Safe to call
    // even on a board with no PSRAM at all -- it is a no-op in that case.
    sysHealthPsramSteer(8192);

    // Arduino's own loop() task already exists by the time setup() runs.
    hLoopTask = xTaskGetHandle("loopTask");
    xTaskCreatePinnedToCore(churnTask, "churn", CHURN_STACK_BYTES, nullptr, 1, &hChurn, 1);

    // sysHealthInit(): register the tasks whose stacks should be watched.
    sysHealthInit(healthTasks, sizeof(healthTasks) / sizeof(healthTasks[0]));

    // sysHealthBootInfo() / sysHealthBootString(): decode esp_reset_reason()
    // and consume the RTC-memory record of any fault from the PREVIOUS
    // session (if the app rebooted itself via sysHealthPrepareReboot()).
    sysHealthBootInfo(g_boot);
    char line[160];
    sysHealthBootString(g_boot, line, sizeof(line));
    Serial.print("Boot info: ");
    Serial.println(line);
    // sysHealthResetName() / sysHealthRebootName(): the same decode used
    // internally by sysHealthBootString(), callable directly too.
    Serial.printf("  esp_reset_reason() decoded: %s\n", sysHealthResetName(g_boot.resetReason));
    if (g_boot.priorReason != SYSH_REBOOT_NONE) {
        Serial.printf("  prior soft-reboot reason: %s\n", sysHealthRebootName(g_boot.priorReason));
    }

    // sysHealthCrashReport(): only produces output if the reset that just
    // happened was a panic AND the build stores core dumps to flash.
    char crash[220];
    if (sysHealthCrashReport(crash, sizeof(crash))) {
        Serial.printf("Crash report: %s\n", crash);
    } else {
        Serial.println("Crash report: none (last reset was not a panic).");
    }

#if DEMO_HAS_TWAI
    setupTwaiDemo();
#else
    Serial.println("driver/twai.h not available on this target; 'k' will report unavailable.");
#endif

    printHelp();
}

void loop() {
    static uint32_t lastAuto = 0;

    if (Serial.available()) {
        char cmd = (char)Serial.read();
        while (Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) Serial.read();

        switch (cmd) {
            case 's': {
                SysHealthSample s;
                sysHealthSample(s);
                printSample(s);
                break;
            }
            case 'c': {
                SysHealthSample s;
                sysHealthSample(s);
                char reason[200];
                uint8_t code, taskIdx;
                uint8_t lvl = sysHealthCheck(s, reason, sizeof(reason), code, taskIdx);
                Serial.printf("check: level=%u (0=ok 1=warn 2=fault) reason=\"%s\"\n", lvl, reason);
                break;
            }
            case 'f': {
                Serial.println("Simulating a LOW_HEAP fault: sysHealthPrepareReboot() + esp_restart() ...");
                Serial.println("Watch the 'Boot info' line after reboot report this as the prior reason.");
                Serial.flush();
                SysHealthSample s;
                sysHealthSample(s);
                sysHealthPrepareReboot(SYSH_REBOOT_LOW_HEAP, 0xFF, s);
                delay(100);
                esp_restart();
                break;
            }
            case 't': {
                Serial.println("Simulating a LOW_STACK fault on task 'churn' (index 1): "
                                "sysHealthPrepareReboot() + esp_restart() ...");
                Serial.println("Watch the 'Boot info' line after reboot report this as the prior reason.");
                Serial.flush();
                SysHealthSample s;
                sysHealthSample(s);
                sysHealthPrepareReboot(SYSH_REBOOT_LOW_STACK, 1, s);
                delay(100);
                esp_restart();
                break;
            }
            case 'p': {
                sysHealthPsramSteer(8192);
                Serial.println("sysHealthPsramSteer(8192) called again (idempotent).");
                break;
            }
            case 'r': {
                char crash[220];
                if (sysHealthCrashReport(crash, sizeof(crash))) {
                    Serial.printf("Crash report: %s\n", crash);
                } else {
                    Serial.println("Crash report: none available (last reset was not a panic).");
                }
                break;
            }
            case 'k': {
#if DEMO_HAS_TWAI
                SysHealthCan can;
                if (sysHealthCanStats(can)) {
                    Serial.printf("TWAI: state=%u tx_err=%u rx_err=%u rx_missed=%lu bus_err=%lu "
                                  "arb_lost=%lu tx_failed=%lu\n",
                                  can.state, can.txErr, can.rxErr, (unsigned long)can.rxMissed,
                                  (unsigned long)can.busErr, (unsigned long)can.arbLost,
                                  (unsigned long)can.txFailed);
                } else {
                    Serial.println("TWAI: driver not running.");
                }
#else
                Serial.println("driver/twai.h not available on this target.");
#endif
                break;
            }
            case 'h':
                printHelp();
                break;
            default:
                break;
        }
    }

    // The pattern a real application follows: sample + check on a timer, act
    // only on warn/fault. See the library README's "Minimal use" section.
    if (millis() - lastAuto > 5000) {
        lastAuto = millis();
        SysHealthSample s;
        sysHealthSample(s);
        char reason[200];
        uint8_t code, taskIdx;
        uint8_t lvl = sysHealthCheck(s, reason, sizeof(reason), code, taskIdx);
        if (lvl == 1) {
            Serial.printf("[auto WARN] %s\n", reason);
        } else if (lvl == 2) {
            Serial.printf("[auto FAULT] %s -- rebooting\n", reason);
            Serial.flush();
            sysHealthPrepareReboot(code, taskIdx, s);
            delay(100);
            esp_restart();
        }
    }

    delay(20);
}
