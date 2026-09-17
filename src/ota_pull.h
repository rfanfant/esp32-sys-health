#pragma once
// ─── ota_pull.h — pull-model OTA for Arduino-ESP32 (core 2.x / 3.x) ──────────
//
// Header-only, transport-agnostic. Lifted from ESP32-InternetMonitor v4.3
// (docs/OTA.md) so the greenhouse master and the monitor share ONE copy.
// This header does the mechanics — manifest fetch / parse / verify, version
// compare, streaming download into the inactive slot with SHA-256 checked
// BEFORE commit, rollback bookkeeping — and leaves policy to the caller: when
// to poll, whether the device is idle enough to apply, what "validated" means,
// and how to report (Telegram, MQTT, serial).
//
// Manifest (flat JSON; tools/ota_publish.py writes it):
//   { "version": "2.5.0",
//     "url":     "http://host/dir/name-2.5.0.bin",
//     "sha256":  "<64 hex>",
//     "size":    1086054,
//     "hmac":    "<64 hex>" }   // optional: HMAC-SHA256 with the shared key over
//                              //   version "\n" url "\n" sha256 "\n" size
//                              // REQUIRED when the device is configured with a
//                              // key; ignored when it is not.
//
// Boot / validation contract (what keeps a bad image from sticking):
//   * This header defines verifyRollbackLater() to return true, so the Arduino
//     core does NOT mark a PENDING_VERIFY image valid before setup() (found on
//     the InternetMonitor bench: without it the rollback net never arms).
//     Include the header from exactly ONE translation unit.
//   * Call otaPullBootCheck() once in setup(). If it reports pendingVerify, the
//     caller must call otaPullMarkBootValid() once its own "this build works"
//     criteria are met, or otaPullRollbackNow() when it decides they never
//     will be. Any reset before that (panic, WDT, brownout) rolls back in the
//     bootloader (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on in the prebuilt
//     Arduino bootloaders this has been tested with).
//   * A version that rolled back is blacklisted in NVS and refused until the
//     manifest offers a different version. Without this the surviving image
//     would see the bad version as "newer" at its next poll, forever.
//
// Versions: up to four integers are read from the leading token of the
// string, so "2.5.0", "v4.3-s9" and "2.5.0 (2026-09-17)" all compare as
// expected. A string with no digits is never "newer" (malformed manifest =
// no-op). Missing trailing components count as 0.
//
// Configuration (define before including to override):
//   OTA_PULL_HTTP_TIMEOUT_MS   connect / stall limit                (default 15000)
//   OTA_PULL_NVS_NAMESPACE     Preferences namespace                (default "otapull")
//   OTA_PULL_NO_ROLLBACK_HOOK  define to keep verifyRollbackLater() out of here
//
// Task safety: otaPullFetchManifest() and otaPullDownloadAndStage() may run in
// a low-priority task; NVS access uses a private Preferences instance (the NVS
// API is internally locked). Detail strings are written with single snprintf
// calls; a torn read is diagnostic only.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#ifndef OTA_PULL_HTTP_TIMEOUT_MS
#define OTA_PULL_HTTP_TIMEOUT_MS 15000
#endif
#ifndef OTA_PULL_NVS_NAMESPACE
#define OTA_PULL_NVS_NAMESPACE "otapull"
#endif

#ifndef OTA_PULL_NO_ROLLBACK_HOOK
// KEEP THE ROLLBACK NET ARMED. esp32-hal-misc.c calls this weak hook during
// startup; its default (false) marks a PENDING_VERIFY image valid before
// setup(), which would let a bad image cancel its own rollback just by booting.
extern "C" bool verifyRollbackLater() { return true; }
#endif

// ── Types ───────────────────────────────────────────────────────────────────

enum OtaPullState : uint8_t {
    OTA_PULL_IDLE = 0,           // nothing known yet / last poll inconclusive (host down, WiFi down)
    OTA_PULL_UP_TO_DATE,         // manifest seen, not newer than the running image
    OTA_PULL_AVAILABLE,          // newer image on offer
    OTA_PULL_WAITING,            // newer image on offer, caller's apply gate says not now
    OTA_PULL_DOWNLOADING,        // streaming into the inactive slot
    OTA_PULL_VERIFIED,           // written + SHA-256 verified + slot selected; restart pending
    OTA_PULL_FAILED,             // download / verify failed; running image untouched
    OTA_PULL_REFUSED,            // offered version is blacklisted or manifest failed HMAC
    OTA_PULL_PENDING_VALIDATION, // this boot is a new image awaiting the caller's validation
    OTA_PULL_VALIDATED,          // caller confirmed this image; rollback cancelled
    OTA_PULL_ROLLED_BACK         // this boot is the bootloader's rollback from a failed image
};

static inline const char* otaPullStateName(OtaPullState s) {
    switch (s) {
        case OTA_PULL_IDLE:               return "idle";
        case OTA_PULL_UP_TO_DATE:         return "up_to_date";
        case OTA_PULL_AVAILABLE:          return "available";
        case OTA_PULL_WAITING:            return "waiting_idle";
        case OTA_PULL_DOWNLOADING:        return "downloading";
        case OTA_PULL_VERIFIED:           return "verified";
        case OTA_PULL_FAILED:             return "failed";
        case OTA_PULL_REFUSED:            return "refused";
        case OTA_PULL_PENDING_VALIDATION: return "pending_validation";
        case OTA_PULL_VALIDATED:          return "validated";
        case OTA_PULL_ROLLED_BACK:        return "rolled_back";
    }
    return "?";
}

struct OtaPullConfig {
    const char* manifestUrl;     // "" = polling disabled
    const char* hmacKey;         // nullptr or "" = unsigned manifests accepted
    const char* runningVersion;  // the firmware's own version string
};

struct OtaPullManifest {
    char     version[40];
    char     url[200];
    char     sha256[65];
    uint32_t size;               // 0 if the manifest omitted it
};

struct OtaPullBootInfo {
    bool        pendingVerify;   // this image booted PENDING_VERIFY: validate or roll back
    bool        rolledBack;      // the bootloader rolled back from rolledBackFrom
    char        rolledBackFrom[40];
    const char* slot;            // running partition label ("app0"/"app1")
    int         slotState;       // esp_ota_img_states_t of the running slot
};

enum OtaPullFetchResult : uint8_t {
    OTA_PULL_FETCH_OK = 0,
    OTA_PULL_FETCH_DISABLED,     // manifestUrl empty
    OTA_PULL_FETCH_NO_WIFI,
    OTA_PULL_FETCH_HTTP,         // connect / GET failed (detail has the code)
    OTA_PULL_FETCH_PARSE,        // missing fields / too large
    OTA_PULL_FETCH_HMAC          // key configured and the manifest's hmac is missing or wrong
};

typedef void (*OtaPullProgressFn)(uint32_t done, uint32_t total);

// ── Module state ────────────────────────────────────────────────────────────

static bool _otaPullPending = false;

// ── Versions ────────────────────────────────────────────────────────────────

// Read up to 'max' integers from the leading token of s (stops at space or
// '('). Returns how many were found; 0 means unparsable.
static inline int otaPullParseVersion(const char* s, int* out, int max) {
    int n = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && *s != '(' && n < max) {
        if (*s >= '0' && *s <= '9') {
            out[n++] = (int)strtol(s, (char**)&s, 10);
        } else {
            s++;
        }
    }
    return n;
}

// -1, 0, +1. Either side unparsable → 0, so "newer" is never true for junk.
static inline int otaPullCompare(const char* a, const char* b) {
    int va[4] = {0, 0, 0, 0}, vb[4] = {0, 0, 0, 0};
    if (otaPullParseVersion(a, va, 4) == 0 || otaPullParseVersion(b, vb, 4) == 0) return 0;
    for (int i = 0; i < 4; i++) {
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    }
    return 0;
}
static inline bool otaPullIsNewer(const char* candidate, const char* running) {
    return otaPullCompare(candidate, running) > 0;
}
static inline bool otaPullSameVersion(const char* a, const char* b) {
    int va[4] = {0, 0, 0, 0}, vb[4] = {0, 0, 0, 0};
    if (otaPullParseVersion(a, va, 4) == 0 || otaPullParseVersion(b, vb, 4) == 0) return strcmp(a, b) == 0;
    return memcmp(va, vb, sizeof va) == 0;
}

// ── NVS bookkeeping ("try" = version being installed, "bad" = blacklisted) ──

static inline String _otaPullNvsGet(const char* key) {
    Preferences p;
    if (!p.begin(OTA_PULL_NVS_NAMESPACE, true)) return String("");
    String v = p.isKey(key) ? p.getString(key, "") : String("");
    p.end();
    return v;
}
static inline void _otaPullNvsSet(const char* key, const char* value) {   // "" removes
    Preferences p;
    if (!p.begin(OTA_PULL_NVS_NAMESPACE, false)) return;
    if (value && value[0]) p.putString(key, value);
    else if (p.isKey(key)) p.remove(key);
    p.end();
}

// ── Boot / validation ───────────────────────────────────────────────────────

// Once in setup(). Detects (a) a just-installed image that must be validated,
// (b) a boot that is the bootloader's rollback from a version we tried.
static inline void otaPullBootCheck(const OtaPullConfig& cfg, OtaPullBootInfo& b) {
    memset(&b, 0, sizeof b);
    b.slot = "?";
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running) {
        b.slot = running->label;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            b.pendingVerify = true;
            _otaPullPending = true;
        }
    }
    b.slotState = (int)st;

    String tried = _otaPullNvsGet("try");
    if (tried.length() > 0 && !otaPullSameVersion(tried.c_str(), cfg.runningVersion)) {
        // We are not the image we tried to install: either the bootloader
        // rolled back (an invalid partition exists) or the install never
        // rebooted. Only the former earns a blacklist entry.
        if (esp_ota_get_last_invalid_partition() != nullptr) {
            _otaPullNvsSet("bad", tried.c_str());
            b.rolledBack = true;
            strlcpy(b.rolledBackFrom, tried.c_str(), sizeof b.rolledBackFrom);
        }
        _otaPullNvsSet("try", "");
    }
}

static inline bool otaPullPending() { return _otaPullPending; }

// Cancel the bootloader rollback for this image. Returns true if this call
// confirmed it (false if nothing was pending or the IDF call failed).
static inline bool otaPullMarkBootValid() {
    if (!_otaPullPending) return false;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    _otaPullPending = false;
    _otaPullNvsSet("try", "");   // installed and confirmed: nothing to blacklist
    return err == ESP_OK;
}

// Give up on this image: the bootloader boots the previous slot. "try" is
// left in NVS so the next boot blacklists this version. Does not return.
static inline void otaPullRollbackNow() {
    esp_ota_mark_app_invalid_rollback_and_reboot();
    for (;;) delay(1000);
}

// Blacklist helpers for the caller's poll policy.
static inline bool otaPullIsBlacklisted(const char* version) {
    String bad = _otaPullNvsGet("bad");
    return bad.length() > 0 && otaPullSameVersion(bad.c_str(), version);
}
// A different version is on offer: forget the old failure.
static inline void otaPullForgetBlacklistUnless(const char* version) {
    String bad = _otaPullNvsGet("bad");
    if (bad.length() > 0 && !otaPullSameVersion(bad.c_str(), version)) _otaPullNvsSet("bad", "");
}

// ── Manifest ────────────────────────────────────────────────────────────────

// Extract "key": "value" from a small flat JSON object. No escaped quotes.
static inline bool _otaPullJsonString(const String& json, const char* key, char* out, size_t n) {
    String k = String("\"") + key + "\"";
    int i = json.indexOf(k);               if (i < 0) return false;
    i = json.indexOf(':', i + k.length()); if (i < 0) return false;
    i = json.indexOf('"', i);              if (i < 0) return false;
    int j = json.indexOf('"', i + 1);      if (j < 0) return false;
    if ((size_t)(j - i - 1) >= n) return false;
    json.substring(i + 1, j).toCharArray(out, n);
    return true;
}
static inline bool _otaPullJsonNumber(const String& json, const char* key, uint32_t& out) {
    String k = String("\"") + key + "\"";
    int i = json.indexOf(k);               if (i < 0) return false;
    i = json.indexOf(':', i + k.length()); if (i < 0) return false;
    i++;
    while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
    if (i >= (int)json.length() || json[i] < '0' || json[i] > '9') return false;
    out = (uint32_t)strtoul(json.c_str() + i, nullptr, 10);
    return true;
}
static inline bool _otaPullHexToBytes(const char* hex, uint8_t* out, size_t n) {
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}
static inline void _otaPullDetail(char* detail, size_t n, const char* fmt, ...) {
    if (!detail || n == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(detail, n, fmt, ap);
    va_end(ap);
}

// HMAC-SHA256(key, version "\n" url "\n" sha256 "\n" size) as lowercase hex.
static inline void otaPullManifestHmac(const char* key, const OtaPullManifest& m, char out[65]) {
    char msg[400];
    snprintf(msg, sizeof msg, "%s\n%s\n%s\n%lu", m.version, m.url, m.sha256, (unsigned long)m.size);
    uint8_t mac[32] = {0};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, (const uint8_t*)key, strlen(key), (const uint8_t*)msg, strlen(msg), mac);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", mac[i]);
    out[64] = '\0';
}

// GET + parse + (if a key is configured) HMAC-verify the manifest.
static inline OtaPullFetchResult otaPullFetchManifest(const OtaPullConfig& cfg, OtaPullManifest& m,
                                                      char* detail, size_t n) {
    memset(&m, 0, sizeof m);
    if (!cfg.manifestUrl || !cfg.manifestUrl[0]) { _otaPullDetail(detail, n, "manifest URL empty; polling disabled"); return OTA_PULL_FETCH_DISABLED; }
    if (WiFi.status() != WL_CONNECTED)           { _otaPullDetail(detail, n, "poll skipped: WiFi down");          return OTA_PULL_FETCH_NO_WIFI; }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(OTA_PULL_HTTP_TIMEOUT_MS);
    http.setTimeout(OTA_PULL_HTTP_TIMEOUT_MS);
    if (!http.begin(client, cfg.manifestUrl)) { _otaPullDetail(detail, n, "bad manifest URL"); return OTA_PULL_FETCH_HTTP; }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        _otaPullDetail(detail, n, "manifest GET failed (%d); host down?", code);
        return OTA_PULL_FETCH_HTTP;
    }
    String body = http.getString();
    http.end();
    if (body.length() > 1024) { _otaPullDetail(detail, n, "manifest too large (%u B); ignored", (unsigned)body.length()); return OTA_PULL_FETCH_PARSE; }

    if (!_otaPullJsonString(body, "version", m.version, sizeof m.version) ||
        !_otaPullJsonString(body, "url",     m.url,     sizeof m.url)     ||
        !_otaPullJsonString(body, "sha256",  m.sha256,  sizeof m.sha256)) {
        _otaPullDetail(detail, n, "manifest missing version/url/sha256; ignored");
        return OTA_PULL_FETCH_PARSE;
    }
    if (!_otaPullJsonNumber(body, "size", m.size)) m.size = 0;

    if (cfg.hmacKey && cfg.hmacKey[0]) {
        char given[80];
        if (!_otaPullJsonString(body, "hmac", given, sizeof given)) {
            _otaPullDetail(detail, n, "%s: manifest is unsigned but a key is configured; refused", m.version);
            return OTA_PULL_FETCH_HMAC;
        }
        char want[65];
        otaPullManifestHmac(cfg.hmacKey, m, want);
        if (strlen(given) != 64 || strcasecmp(given, want) != 0) {
            _otaPullDetail(detail, n, "%s: manifest HMAC mismatch; refused", m.version);
            return OTA_PULL_FETCH_HMAC;
        }
    }
    _otaPullDetail(detail, n, "manifest %s (running %s)", m.version, cfg.runningVersion);
    return OTA_PULL_FETCH_OK;
}

// Newer than us and not blacklisted. Clears a stale blacklist when the offer
// changed. 'detail' explains a false result.
static inline bool otaPullShouldInstall(const OtaPullConfig& cfg, const OtaPullManifest& m, char* detail, size_t n) {
    if (!otaPullIsNewer(m.version, cfg.runningVersion)) {
        _otaPullDetail(detail, n, "up to date (manifest %s, running %s)", m.version, cfg.runningVersion);
        return false;
    }
    if (otaPullIsBlacklisted(m.version)) {
        _otaPullDetail(detail, n, "%s offered again but it rolled back last time; refusing until the manifest changes", m.version);
        return false;
    }
    otaPullForgetBlacklistUnless(m.version);
    _otaPullDetail(detail, n, "%s is newer than %s", m.version, cfg.runningVersion);
    return true;
}

// ── Download ────────────────────────────────────────────────────────────────

// Stream the image into the inactive slot, hashing as it goes. Only if the
// hash matches is Update.end() called, which selects the new slot for the
// next boot. Any failure leaves the running image untouched. On success the
// caller does its own safe-state work and esp_restart()s; "try" is recorded
// first so a rollback can be attributed on the next boot.
static inline bool otaPullDownloadAndStage(const OtaPullManifest& m, OtaPullProgressFn progress,
                                           char* detail, size_t n) {
    uint8_t want[32];
    if (!_otaPullHexToBytes(m.sha256, want, sizeof want)) {
        _otaPullDetail(detail, n, "%s rejected: manifest sha256 is not 64 hex chars", m.version);
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) { _otaPullDetail(detail, n, "%s: WiFi down", m.version); return false; }

    _otaPullNvsSet("try", m.version);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(OTA_PULL_HTTP_TIMEOUT_MS);
    http.setTimeout(OTA_PULL_HTTP_TIMEOUT_MS);
    if (!http.begin(client, m.url)) { _otaPullDetail(detail, n, "%s: bad image URL", m.version); return false; }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        _otaPullDetail(detail, n, "%s: image GET failed (%d)", m.version, code);
        return false;
    }
    int len = http.getSize();
    if (len <= 0) {
        http.end();
        _otaPullDetail(detail, n, "%s: image has no Content-Length; refusing to stream an unknown size", m.version);
        return false;
    }
    if (m.size && (uint32_t)len != m.size) {
        http.end();
        _otaPullDetail(detail, n, "%s: Content-Length %d != manifest size %lu; refused", m.version, len, (unsigned long)m.size);
        return false;
    }
    // Update.begin() checks the image fits the next slot; the first write()
    // checks the ESP image magic, so a non-firmware file fails early.
    if (!Update.begin((size_t)len, U_FLASH)) {
        http.end();
        _otaPullDetail(detail, n, "%s: Update.begin failed (%s), size %d", m.version, Update.errorString(), len);
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    static uint8_t buf[4096];               // static: off the task stack
    WiFiClient* stream = http.getStreamPtr();
    size_t   total    = 0;
    uint32_t lastData = millis();
    uint32_t lastProg = 0;
    bool     ok       = true;
    while (total < (size_t)len) {
        if (!http.connected() && stream->available() == 0) { ok = false; break; }
        size_t wantN = (size_t)len - total;
        if (wantN > sizeof buf) wantN = sizeof buf;
        int got = stream->read(buf, wantN);
        if (got <= 0) {
            if (millis() - lastData > OTA_PULL_HTTP_TIMEOUT_MS) { ok = false; break; }
            delay(10);
            continue;
        }
        lastData = millis();
        if (Update.write(buf, (size_t)got) != (size_t)got) { ok = false; break; }
        mbedtls_sha256_update(&sha, buf, (size_t)got);
        total += (size_t)got;
        if (progress && millis() - lastProg >= 500) { lastProg = millis(); progress((uint32_t)total, (uint32_t)len); }
    }
    http.end();

    uint8_t gotHash[32];
    mbedtls_sha256_finish(&sha, gotHash);
    mbedtls_sha256_free(&sha);

    if (!ok || total != (size_t)len) {
        Update.abort();
        _otaPullDetail(detail, n, "%s: download failed at %u of %d B (%s); running image untouched",
                       m.version, (unsigned)total, len, Update.hasError() ? Update.errorString() : "stalled/disconnected");
        return false;
    }
    if (memcmp(gotHash, want, sizeof gotHash) != 0) {
        Update.abort();
        _otaPullDetail(detail, n, "%s: SHA-256 MISMATCH, image rejected before commit; running image untouched", m.version);
        return false;
    }
    if (!Update.end()) {
        _otaPullDetail(detail, n, "%s: Update.end failed (%s); running image untouched", m.version, Update.errorString());
        return false;
    }
    if (progress) progress((uint32_t)total, (uint32_t)len);
    _otaPullDetail(detail, n, "%s written to the inactive slot, %u B, SHA-256 verified", m.version, (unsigned)total);
    return true;
}
