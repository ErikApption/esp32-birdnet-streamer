#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include <time.h>
#include <sunset.h>
#include <esp_system.h>
#include <opus.h>

// Onboard LED
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48
#endif

// ─── I2S Configuration ───────────────────────────────────────────────────────
#define I2S_WS_PIN        5   // Word Select (LRCLK)
#define I2S_SD_PIN        6   // Serial Data (DOUT from mic)
#define I2S_SCK_PIN       4   // Serial Clock (BCLK)

// ─── Microphone Power Pin ────────────────────────────────────────────────────
#define MIC_POWER_PIN     10  // GPIO powering INMP441 VDD (~1.4 mA)

#define DEFAULT_SAMPLE_RATE 48000
#define DMA_BUF_COUNT     8
#define DMA_BUF_LEN       1024

// ─── UDP Configuration ───────────────────────────────────────────────────────
#define DEFAULT_UDP_HOST  "192.168.1.100"
#define DEFAULT_UDP_PORT  4000
#define UDP_RETRY_ATTEMPTS     3      // Retries per packet on send failure
#define UDP_RETRY_DELAY_MS     2      // Delay between retries
#define UDP_BACKOFF_BASE_MS    50     // Base backoff after burst failure
#define UDP_BACKOFF_MAX_MS     5000   // Maximum backoff duration
#define UDP_STATS_INTERVAL_MS  (30 * 1000)  // Log UDP stats every 30s

// ─── Opus Encoder Configuration ─────────────────────────────────────────────
#define OPUS_FRAME_MS         60    // 60ms frames — maximum efficiency, fewer packets
#define OPUS_BITRATE_BPS      64000 // 64 kbps — good quality for bird audio
#define OPUS_COMPLEXITY_LEVEL 5     // 0-10, higher = better quality but more CPU
#define OPUS_FRAMES_PER_PACKET 4    // Bundle 4 frames per UDP packet (240ms per packet)

// ─── Burst Transmission Configuration ────────────────────────────────────────
// Buffer encoded packets and send them in bursts to let WiFi radio sleep between
#define BURST_INTERVAL_MS     1000  // Accumulate for 1 second, then burst-send
#define BURST_MAX_PACKETS     8     // Max packets to buffer (8 × 240ms = 1.92s headroom)

// ─── Location defaults (used for sunrise/sunset calculation) ─────────────────
#define DEFAULT_LATITUDE   "45.4215"
#define DEFAULT_LONGITUDE  "-75.6972"
#define DEFAULT_UTC_OFFSET "-5"

// ─── NTP Configuration ──────────────────────────────────────────────────────
#define NTP_SERVER        "pool.ntp.org"

// ─── Power Monitor Configuration ─────────────────────────────────────────────
#define MONITOR_EN_PIN    7   // GPIO to enable voltage dividers (MOSFET gate)
#define VBAT_ADC_PIN      8   // ADC1_CH7 — battery voltage
#define VSOL_ADC_PIN      9   // ADC1_CH8 — solar voltage
#define ADC_VREF          3.1f
#define ADC_RESOLUTION    4095.0f
#define ADC_SAMPLES       16
#define POWER_READ_INTERVAL_MS  (60 * 1000)  // Read every 60 seconds

// Telemetry trailer: appended to audio packets (no separate telemetry socket needed)
// Format: magic "TL" (2 bytes) + telemetry payload (20 bytes) = 22 bytes total
// Attached to the last audio packet in each burst when telemetry data is fresh.
#define TELEMETRY_MAGIC_0   0x54  // 'T'
#define TELEMETRY_MAGIC_1   0x4C  // 'L'
#define TELEMETRY_TRAILER_SIZE 22  // 2 magic + 2 bat + 2 sol + 2 rssi + 4 sent + 4 errors + 4 dropped + 1 wifi + 1 consec_fails

// ─── HTTP Config Server ──────────────────────────────────────────────────────
#define HTTP_PORT         80

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiUDP udp;          // Audio streaming — used exclusively by audioTask on core 1
WebServer server(HTTP_PORT);
Preferences prefs;

char udpHost[64]    = DEFAULT_UDP_HOST;
char udpPort[6]     = "4000";
char latitude[12]   = DEFAULT_LATITUDE;
char longitude[12]  = DEFAULT_LONGITUDE;
char utcOffset[4]   = DEFAULT_UTC_OFFSET;
bool sleepEnabled   = false;

// ─── Debug Mode ──────────────────────────────────────────────────────────────
// Define FORCE_DEBUG_MODE at compile time to permanently disable deep sleep,
// overriding the NVS setting. Useful during development/bench testing.
// Can be set in platformio.ini: build_flags = -DFORCE_DEBUG_MODE
#ifndef FORCE_DEBUG_MODE
#define FORCE_DEBUG_MODE 0
#endif

bool debugMode      = FORCE_DEBUG_MODE;  // When true, deep sleep is completely disabled

// ─── Diagnostic Mode State ────────────────────────────────────────────────────
bool diagnosticMode = false;  // true when /diag/start has been called
unsigned long lastDiagTelemetryTime = 0;
#define DIAG_TELEMETRY_INTERVAL_MS 2000  // Send telemetry every 2s in diagnostic mode

// ─── I2S Signal Monitor State ────────────────────────────────────────────────
volatile uint32_t i2sFramesTotal = 0;   // total DMA frames processed in current window
volatile uint32_t i2sFramesEmpty = 0;   // DMA frames with no meaningful audio content
volatile uint32_t opusFramesEncoded = 0; // Opus frames encoded in current window
volatile uint32_t opusFramesSilent = 0;  // Opus frames that encoded as near-silent (very few bytes)
unsigned long lastSignalCheckTime = 0;
#define SIGNAL_CHECK_INTERVAL_MS 500  // LED update interval
#define OPUS_SILENT_THRESHOLD 80      // Opus frames <= this many bytes are silence/near-silence
                                      // At 64kbps/60ms, real audio encodes to ~480 bytes.
                                      // DTX silence = 2-3 bytes, comfort noise = 10-40 bytes,
                                      // noise from floating/shorted pin = 40-80 bytes.
                                      // Anything above ~80 bytes has meaningful audio content.

// ─── Power Monitor State ─────────────────────────────────────────────────────
float lastBatteryVoltage = 0.0f;  // raw ADC voltage (before divider scaling)
float lastSolarVoltage   = 0.0f;  // raw ADC voltage (before divider scaling)
unsigned long lastPowerReadTime = 0;

// ─── Telemetry Trailer State ─────────────────────────────────────────────────
// Telemetry is assembled in loop() on core 0, then attached to the next audio
// burst by the audioTask on core 1. A volatile flag signals fresh data.
volatile bool telemetryPending = false;
uint8_t telemetryTrailer[TELEMETRY_TRAILER_SIZE];  // pre-built trailer bytes

// ─── UDP Target (used by streaming) ──────────────────────────────────────────
IPAddress resolvedUdpAddr;
unsigned long lastResolveTime = 0;
unsigned long udpSendErrors = 0;
unsigned long udpSendSuccess = 0;
unsigned long udpBurstCount = 0;
unsigned long udpPacketsDropped = 0;       // packets lost due to ring buffer overflow
unsigned long udpConsecutiveFails = 0;     // current streak of failed sends
unsigned long udpBackoffMs = 0;            // current dynamic backoff in ms
unsigned long lastUdpStatsTime = 0;        // last time UDP stats were logged
unsigned long lastBurstFailTime = 0;       // time of last burst failure (for backoff)
bool udpFirstPacketLogged = false;

uint32_t sampleRate = DEFAULT_SAMPLE_RATE;
char sampleRateStr[8] = "48000";
bool configured     = false;  // true once a config has been received (persisted in NVS)

static int32_t i2sReadBuffer[DMA_BUF_LEN];  // 32-bit I2S read buffer
static int16_t i2sBuffer[DMA_BUF_LEN];     // 16-bit samples after bit-shift conversion
static i2s_chan_handle_t rx_handle = NULL;  // New I2S driver channel handle

// ─── Opus Encoder Globals ────────────────────────────────────────────────────
OpusEncoder *opusEncoder = nullptr;
int opusFrameSamples = 0;            // samples per Opus frame (sampleRate * OPUS_FRAME_MS / 1000)
int16_t *opusInputBuffer = nullptr;  // accumulator for one Opus frame (PSRAM — large, write-only)
int opusInputIndex = 0;              // current fill level in opusInputBuffer
uint8_t *opusOutputBuffer = nullptr; // encoded output buffer (internal RAM — fast access)
#define OPUS_MAX_FRAME_SIZE 1275     // max Opus frame size per spec

// Multi-frame UDP packet buffer: [header][len1][frame1][len2][frame2]...
// Header: 4 bytes — [0x4F 0x50] magic, [1 byte frame count], [1 byte sequence number]
// Each frame: 2 bytes big-endian length + encoded data
uint8_t *udpPacketBuffer = nullptr;  // assembled multi-frame UDP packet (internal RAM)
int udpPacketOffset = 0;             // current write position in packet buffer
int udpPacketFrameCount = 0;         // frames accumulated in current packet
uint8_t udpSequenceNumber = 0;       // wrapping sequence counter for packet ordering
#define UDP_PACKET_HEADER_SIZE 4     // magic(2) + frame_count(1) + seq(1)
#define UDP_PACKET_BUF_SIZE 1400     // stay well under MTU (1500-20-8=1472; +22 trailer = max 1422)

// ─── Burst Buffer (ring of completed packets awaiting transmission) ──────────
struct BurstPacket {
    uint8_t data[UDP_PACKET_BUF_SIZE];
    int length;
};
BurstPacket *burstBuffer = nullptr;  // ring buffer in PSRAM
volatile int burstWriteIdx = 0;      // next slot to write
volatile int burstReadIdx = 0;       // next slot to send
unsigned long lastBurstTime = 0;     // timestamp of last burst send

// ─── mDNS Hostname ───────────────────────────────────────────────────────────
#define MDNS_HOSTNAME     "esp32-birdnet"
#define MDNS_SERVICE      "birdnet"

// ─── Forward declarations ────────────────────────────────────────────────────
void wifiInit();
void otaInit();
void i2sInit();
void opusInit();
void udpInit();
void flushUdpPacket(uint16_t port);
void burstSendPackets();
void logUdpStats();
void httpInit();
void mdnsInit();
void startStreaming();
IPAddress resolveHost(const char* hostname);
void streamAudio();
void enterDeepSleep(uint64_t sleepSeconds);
bool isWithinActiveWindow();
void syncTime();
void loadSettings();
void saveSettings();
void getSunTimes(int year, int month, int day, double &sunriseMin, double &sunsetMin);
void powerMonitorInit();
void readPowerMonitor();
void sendTelemetryPacket();
void handleDiagStart();
void handleDiagStop();
void updateSignalLed();
float readAdcVoltage(int pin);

// ─── Persistent Settings (NVS) ───────────────────────────────────────────────
void loadSettings() {
    // Use read-write mode so the namespace is created on first boot
    if (!prefs.begin("birdnet", false)) {
        Serial.println("[NVS] Failed to open preferences — using defaults");
        return;
    }

    // On first boot, keys won't exist yet — seed them with defaults
    if (!prefs.isKey("udpHost")) {
        Serial.println("[NVS] First boot — writing default settings");
        prefs.putString("udpHost", DEFAULT_UDP_HOST);
        prefs.putString("udpPort", "4000");
        prefs.putString("latitude", DEFAULT_LATITUDE);
        prefs.putString("longitude", DEFAULT_LONGITUDE);
        prefs.putString("utcOffset", DEFAULT_UTC_OFFSET);
        prefs.putBool("sleepOn", true);
        prefs.putUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    }

    String h = prefs.getString("udpHost", DEFAULT_UDP_HOST);
    String p = prefs.getString("udpPort", "4000");
    String la = prefs.getString("latitude", DEFAULT_LATITUDE);
    String lo = prefs.getString("longitude", DEFAULT_LONGITUDE);
    String uo = prefs.getString("utcOffset", DEFAULT_UTC_OFFSET);
    sleepEnabled = prefs.getBool("sleepOn", true);
    sampleRate = prefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    configured = prefs.getBool("configured", false);
    debugMode = prefs.getBool("debugMode", false) || FORCE_DEBUG_MODE;
    prefs.end();

    strncpy(udpHost, h.c_str(), sizeof(udpHost) - 1);
    strncpy(udpPort, p.c_str(), sizeof(udpPort) - 1);
    strncpy(latitude, la.c_str(), sizeof(latitude) - 1);
    strncpy(longitude, lo.c_str(), sizeof(longitude) - 1);
    strncpy(utcOffset, uo.c_str(), sizeof(utcOffset) - 1);
    snprintf(sampleRateStr, sizeof(sampleRateStr), "%lu", (unsigned long)sampleRate);

    Serial.printf("[NVS] Configured: %s\n", configured ? "yes" : "no");
}

void saveSettings() {
    prefs.begin("birdnet", false); // read-write
    prefs.putString("udpHost", udpHost);
    prefs.putString("udpPort", udpPort);
    prefs.putString("latitude", latitude);
    prefs.putString("longitude", longitude);
    prefs.putString("utcOffset", utcOffset);
    prefs.putBool("sleepOn", sleepEnabled);
    prefs.putUInt("sampleRate", sampleRate);
    prefs.putBool("configured", configured);
    prefs.putBool("debugMode", debugMode);
    prefs.end();
}

// ─── WiFiManager save callback ───────────────────────────────────────────────
void saveParamsCallback() {
    configured = true;
    Serial.println("[WiFiManager] Parameters saved — device configured");
}

// ─── HTTP Configuration Server ───────────────────────────────────────────────
void handleGetConfig() {
    String json = "{";
    json += "\"udp_host\":\"" + String(udpHost) + "\",";
    json += "\"udp_port\":" + String(udpPort) + ",";
    json += "\"sample_rate\":" + String(sampleRate) + ",";
    json += "\"latitude\":\"" + String(latitude) + "\",";
    json += "\"longitude\":\"" + String(longitude) + "\",";
    json += "\"utc_offset\":\"" + String(utcOffset) + "\",";
    json += "\"sleep_enabled\":" + String(sleepEnabled ? "true" : "false") + ",";
    json += "\"debug_mode\":" + String(debugMode ? "true" : "false") + ",";
    json += "\"configured\":" + String(configured ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
}

void handlePostConfig() {
    bool changed = false;
    bool needsReboot = false;

    if (server.hasArg("udp_host")) {
        strncpy(udpHost, server.arg("udp_host").c_str(), sizeof(udpHost) - 1);
        changed = true;
    }
    if (server.hasArg("udp_port")) {
        strncpy(udpPort, server.arg("udp_port").c_str(), sizeof(udpPort) - 1);
        changed = true;
    }
    if (server.hasArg("sample_rate")) {
        uint32_t newRate = server.arg("sample_rate").toInt();
        // Validate: only allow common I2S sample rates
        if (newRate == 8000 || newRate == 16000 || newRate == 22050 ||
            newRate == 32000 || newRate == 44100 || newRate == 48000) {
            if (newRate != sampleRate) {
                sampleRate = newRate;
                snprintf(sampleRateStr, sizeof(sampleRateStr), "%lu", (unsigned long)sampleRate);
                changed = true;
                needsReboot = true;
            }
        }
    }
    if (server.hasArg("latitude")) {
        strncpy(latitude, server.arg("latitude").c_str(), sizeof(latitude) - 1);
        changed = true;
    }
    if (server.hasArg("longitude")) {
        strncpy(longitude, server.arg("longitude").c_str(), sizeof(longitude) - 1);
        changed = true;
    }
    if (server.hasArg("utc_offset")) {
        strncpy(utcOffset, server.arg("utc_offset").c_str(), sizeof(utcOffset) - 1);
        changed = true;
    }
    if (server.hasArg("sleep_enabled")) {
        String val = server.arg("sleep_enabled");
        sleepEnabled = (val == "true" || val == "1");
        changed = true;
    }
    if (server.hasArg("debug_mode")) {
        String val = server.arg("debug_mode");
        debugMode = (val == "true" || val == "1");
        changed = true;
    }

    if (changed) {
        configured = true;
        saveSettings();
        Serial.printf("[HTTP] Config updated — UDP: %s:%s, rate: %lu, sleep: %s, debug: %s\n",
            udpHost, udpPort, (unsigned long)sampleRate, sleepEnabled ? "on" : "off",
            debugMode ? "on" : "off");
    }

    if (needsReboot) {
        String json = "{\"message\":\"Sample rate changed to " + String(sampleRate) + " Hz. Rebooting...\"}";
        server.send(200, "application/json", json);
        delay(500);
        ESP.restart();
    } else {
        handleGetConfig();
    }
}

void handleGetStatus() {
    struct tm timeinfo;
    String timeStr = "unknown";
    String sunriseStr = "—";
    String sunsetStr = "—";

    if (getLocalTime(&timeinfo)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        timeStr = String(buf);

        double sunriseMin, sunsetMin;
        getSunTimes(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    sunriseMin, sunsetMin);
        int srH = (int)(sunriseMin / 60);
        int srM = (int)sunriseMin % 60;
        int ssH = (int)(sunsetMin / 60);
        int ssM = (int)sunsetMin % 60;
        char srBuf[8], ssBuf[8];
        snprintf(srBuf, sizeof(srBuf), "%02d:%02d", srH, srM);
        snprintf(ssBuf, sizeof(ssBuf), "%02d:%02d", ssH, ssM);
        sunriseStr = String(srBuf);
        sunsetStr = String(ssBuf);
    }

    String json = "{";
    json += "\"time\":\"" + timeStr + "\",";
    json += "\"sunrise\":\"" + sunriseStr + "\",";
    json += "\"sunset\":\"" + sunsetStr + "\",";
    json += "\"sleep_enabled\":" + String(sleepEnabled ? "true" : "false") + ",";
    json += "\"debug_mode\":" + String(debugMode ? "true" : "false") + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"battery_v\":" + String(lastBatteryVoltage, 3) + ",";
    json += "\"solar_v\":" + String(lastSolarVoltage, 3) + ",";
    json += "\"diagnostic\":" + String(diagnosticMode ? "true" : "false") + ",";
    json += "\"udp\":{";
    json += "\"target\":\"" + String(udpHost) + ":" + String(udpPort) + "\",";
    json += "\"resolved\":\"" + resolvedUdpAddr.toString() + "\",";
    json += "\"sent\":" + String(udpSendSuccess) + ",";
    json += "\"errors\":" + String(udpSendErrors) + ",";
    json += "\"dropped\":" + String(udpPacketsDropped) + ",";
    json += "\"consecutive_fails\":" + String(udpConsecutiveFails) + ",";
    json += "\"backoff_ms\":" + String(udpBackoffMs) + ",";
    json += "\"bursts\":" + String(udpBurstCount);
    json += "}";
    json += "}";
    server.send(200, "application/json", json);
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

void handlePostSleep() {
    if (!server.hasArg("minutes")) {
        server.send(400, "application/json", "{\"error\":\"missing 'minutes' parameter\"}");
        return;
    }

    int minutes = server.arg("minutes").toInt();
    if (minutes <= 0 || minutes > 1440) {
        server.send(400, "application/json", "{\"error\":\"'minutes' must be between 1 and 1440\"}");
        return;
    }

    String json = "{\"status\":\"sleeping\",\"minutes\":" + String(minutes) + "}";
    server.send(200, "application/json", json);
    Serial.printf("[HTTP] Sleep requested for %d minutes\n", minutes);

    // Give the HTTP response time to be sent before sleeping
    delay(100);

    enterDeepSleep((uint64_t)minutes * 60);
}

void httpInit() {
    server.on("/config", HTTP_GET, handleGetConfig);
    server.on("/config", HTTP_POST, handlePostConfig);
    server.on("/status", HTTP_GET, handleGetStatus);
    server.on("/sleep", HTTP_POST, handlePostSleep);
    server.on("/diag/start", HTTP_GET, handleDiagStart);
    server.on("/diag/stop", HTTP_GET, handleDiagStop);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.printf("[HTTP] Config server on port %d\n", HTTP_PORT);
}

// ─── mDNS Setup ──────────────────────────────────────────────────────────────
void mdnsInit() {
    Serial.println("[mDNS] Starting mDNS responder...");

    // Give the WiFi stack time to fully stabilize before starting mDNS
    delay(200);

    if (!MDNS.begin(MDNS_HOSTNAME)) {
        Serial.println("[mDNS] Failed to start — retrying once...");
        delay(1000);
        if (!MDNS.begin(MDNS_HOSTNAME)) {
            Serial.println("[mDNS] FAILED to start on retry — mDNS unavailable");
            return;
        }
    }

    // Advertise HTTP config server with descriptive TXT records
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addServiceTxt("http", "tcp", "path", "/config");
    MDNS.addServiceTxt("http", "tcp", "device", "ESP32-BirdNet-Streamer");

    // Advertise custom birdnet service so receivers can discover us
    uint16_t port = (uint16_t)atoi(udpPort);
    MDNS.addService(MDNS_SERVICE, "udp", port);
    MDNS.addServiceTxt(MDNS_SERVICE, "udp", "rate", String(sampleRateStr));
    MDNS.addServiceTxt(MDNS_SERVICE, "udp", "codec", "opus");
    MDNS.addServiceTxt(MDNS_SERVICE, "udp", "frame_ms", String(OPUS_FRAME_MS));
    MDNS.addServiceTxt(MDNS_SERVICE, "udp", "frames_per_pkt", String(OPUS_FRAMES_PER_PACKET));
    MDNS.addServiceTxt(MDNS_SERVICE, "udp", "version", "1.0");

    Serial.printf("[mDNS] Hostname  : %s.local\n", MDNS_HOSTNAME);
    Serial.printf("[mDNS] Services  : http/tcp:%d, %s/udp:%d\n", HTTP_PORT, MDNS_SERVICE, port);
    Serial.printf("[mDNS] Access URL: http://%s.local/config\n", MDNS_HOSTNAME);
}

// ─── Resolve hostname (supports .local mDNS names and regular IPs) ──────────
IPAddress resolveHost(const char* hostname) {
    IPAddress addr;

    // If it's already a numeric IP, parse directly
    if (addr.fromString(hostname)) {
        return addr;
    }

    // Try mDNS resolution for .local hostnames
    String host = String(hostname);
    if (host.endsWith(".local")) {
        // Strip ".local" suffix for MDNS.queryHost
        String name = host.substring(0, host.length() - 6);
        addr = MDNS.queryHost(name.c_str(), 3000); // 3 second timeout
        if (addr != IPAddress(0, 0, 0, 0)) {
            Serial.printf("[mDNS] Resolved %s -> %s\n", hostname, addr.toString().c_str());
            return addr;
        }
        Serial.printf("[mDNS] Failed to resolve %s\n", hostname);
    }

    // Fallback: try DNS resolution
    if (WiFi.hostByName(hostname, addr)) {
        return addr;
    }

    Serial.printf("[Resolve] Cannot resolve %s\n", hostname);
    return IPAddress(0, 0, 0, 0);
}

// ─── Power Monitor ───────────────────────────────────────────────────────────
void powerMonitorInit() {
    pinMode(MONITOR_EN_PIN, OUTPUT);
    digitalWrite(MONITOR_EN_PIN, LOW);  // Dividers OFF by default
    analogSetAttenuation(ADC_11db);     // 0–3.1V range
    analogReadResolution(12);           // 12-bit (0–4095)
    Serial.println("[Power] Monitor initialized");

    // ─── Self-test: diagnose voltage divider wiring ─────────────────────
    Serial.println("[Power] ┌─── DIAGNOSTIC SELF-TEST ───────────────────────┐");

    // Step 1: Read with MOSFET OFF — both ADC pins should read near 0V
    // (no current path through dividers, pins should be at ground potential
    //  through R2/R4 if wired correctly, or floating if ground path is broken)
    digitalWrite(MONITOR_EN_PIN, LOW);
    delay(10);
    float batOff = readAdcVoltage(VBAT_ADC_PIN);
    float solOff = readAdcVoltage(VSOL_ADC_PIN);
    Serial.printf("[Power] │ MOSFET OFF  — bat ADC: %.3fV, sol ADC: %.3fV\n", batOff, solOff);

    if (batOff > 0.1f || solOff > 0.1f) {
        Serial.println("[Power] │ ⚠ PROBLEM: ADC reads >0.1V with MOSFET off!");
        Serial.println("[Power] │   Expected: ~0V (divider has no ground path)");
        Serial.println("[Power] │   If reading near 3.0V: pins are floating (broken GND wiring)");
        Serial.println("[Power] │   If reading 1-2V: possible leakage or wrong pin connection");
    } else {
        Serial.println("[Power] │ ✓ OK — pins near 0V when dividers disabled");
    }

    // Step 2: Read with MOSFET ON — should see actual divided voltages
    digitalWrite(MONITOR_EN_PIN, HIGH);
    delay(5);  // settling time
    float batOn = readAdcVoltage(VBAT_ADC_PIN);
    float solOn = readAdcVoltage(VSOL_ADC_PIN);
    Serial.printf("[Power] │ MOSFET ON   — bat ADC: %.3fV, sol ADC: %.3fV\n", batOn, solOn);

    // Expected battery ADC for 3S NiMH: 1.5V–2.25V (3.0V–4.5V / 2)
    float batReal = batOn * 2.0f;
    float solReal = solOn * 3.2f;
    Serial.printf("[Power] │ Calculated  — bat: %.2fV, sol: %.2fV\n", batReal, solReal);

    if (batReal > 4.5f) {
        Serial.println("[Power] │ ⚠ PROBLEM: Battery reads > 4.5V (impossible for 3S NiMH)");
        Serial.println("[Power] │   Check: is battery sense wire on correct terminal?");
        Serial.println("[Power] │   Check: is MOSFET drain connected to divider ground?");
    } else if (batReal < 2.5f) {
        Serial.println("[Power] │ ⚠ WARNING: Battery reads < 2.5V (cells may be dead)");
    } else {
        Serial.printf("[Power] │ ✓ OK — battery %.2fV is within 3S NiMH range (3.0–4.5V)\n", batReal);
    }

    if (solReal > 0.5f) {
        Serial.printf("[Power] │ ℹ Solar panel reading: %.2fV\n", solReal);
    } else {
        Serial.println("[Power] │ ℹ Solar: no voltage detected (panel disconnected or dark)");
    }

    // Step 3: Check that switching actually changes the reading
    float batDelta = fabsf(batOn - batOff);
    float solDelta = fabsf(solOn - solOff);
    Serial.printf("[Power] │ Delta (on-off) — bat: %.3fV, sol: %.3fV\n", batDelta, solDelta);

    if (batDelta < 0.05f) {
        Serial.println("[Power] │ ⚠ PROBLEM: Battery ADC doesn't change with MOSFET!");
        Serial.println("[Power] │   MOSFET may not be switching, or divider not connected.");
        Serial.println("[Power] │   Check: GPIO 7 → MOSFET gate, drain → R2+R4 bottom, source → GND");
    }

    // Disable dividers
    digitalWrite(MONITOR_EN_PIN, LOW);
    Serial.println("[Power] └────────────────────────────────────────────────┘");
}

float readAdcVoltage(int pin) {
    uint32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        sum += analogRead(pin);
        delayMicroseconds(100);
    }
    float avgRaw = (float)sum / ADC_SAMPLES;
    return (avgRaw / ADC_RESOLUTION) * ADC_VREF;
}

void readPowerMonitor() {
    // Enable voltage dividers (skip if already HIGH due to diagnostic mode)
    if (!diagnosticMode) {
        digitalWrite(MONITOR_EN_PIN, HIGH);
    }
    delayMicroseconds(2000);  // RC settling — use microseconds to avoid triggering RTOS tick

    float vBatAdc = readAdcVoltage(VBAT_ADC_PIN);
    float vSolAdc = readAdcVoltage(VSOL_ADC_PIN);

    // Disable voltage dividers (only if not in diagnostic mode)
    if (!diagnosticMode) {
        digitalWrite(MONITOR_EN_PIN, LOW);
    }

    // Store raw ADC voltages (divider scaling applied by the receiver)
    lastBatteryVoltage = vBatAdc;
    lastSolarVoltage   = vSolAdc;

    Serial.printf("[Power] ADC raw — Battery: %.3fV, Solar: %.3fV\n",
                  lastBatteryVoltage, lastSolarVoltage);
}

void sendTelemetryPacket() {
    if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) return;

    // Build telemetry trailer bytes (will be appended to the next audio burst packet)
    uint16_t batMv = (uint16_t)(lastBatteryVoltage * 1000.0f);
    uint16_t solMv = (uint16_t)(lastSolarVoltage * 1000.0f);
    int16_t rssi = (int16_t)WiFi.RSSI();
    uint32_t sent = (uint32_t)udpSendSuccess;
    uint32_t errors = (uint32_t)udpSendErrors;
    uint32_t dropped = (uint32_t)udpPacketsDropped;
    uint8_t wifiStatus = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    uint8_t consecFails = (uint8_t)min(udpConsecutiveFails, (unsigned long)255);

    telemetryTrailer[0] = TELEMETRY_MAGIC_0;       // 'T'
    telemetryTrailer[1] = TELEMETRY_MAGIC_1;       // 'L'
    telemetryTrailer[2] = (uint8_t)(batMv >> 8);   // battery ADC mV big-endian
    telemetryTrailer[3] = (uint8_t)(batMv & 0xFF);
    telemetryTrailer[4] = (uint8_t)(solMv >> 8);   // solar ADC mV big-endian
    telemetryTrailer[5] = (uint8_t)(solMv & 0xFF);
    telemetryTrailer[6] = (uint8_t)(rssi >> 8);    // RSSI int16 big-endian
    telemetryTrailer[7] = (uint8_t)(rssi & 0xFF);
    telemetryTrailer[8]  = (uint8_t)(sent >> 24);  // UDP sent uint32 big-endian
    telemetryTrailer[9]  = (uint8_t)(sent >> 16);
    telemetryTrailer[10] = (uint8_t)(sent >> 8);
    telemetryTrailer[11] = (uint8_t)(sent & 0xFF);
    telemetryTrailer[12] = (uint8_t)(errors >> 24); // UDP errors uint32 big-endian
    telemetryTrailer[13] = (uint8_t)(errors >> 16);
    telemetryTrailer[14] = (uint8_t)(errors >> 8);
    telemetryTrailer[15] = (uint8_t)(errors & 0xFF);
    telemetryTrailer[16] = (uint8_t)(dropped >> 24); // UDP dropped uint32 big-endian
    telemetryTrailer[17] = (uint8_t)(dropped >> 16);
    telemetryTrailer[18] = (uint8_t)(dropped >> 8);
    telemetryTrailer[19] = (uint8_t)(dropped & 0xFF);
    telemetryTrailer[20] = wifiStatus;              // 1=connected, 0=disconnected
    telemetryTrailer[21] = consecFails;             // consecutive send failures (capped 255)

    // Signal the audio task to attach this trailer to the next burst
    telemetryPending = true;
}

// ─── Diagnostic Mode Endpoints ───────────────────────────────────────────────
void handleDiagStart() {
    diagnosticMode = true;
    // Set voltage monitor pin HIGH for external measurement
    digitalWrite(MONITOR_EN_PIN, HIGH);
    // Enable the onboard LED for signal indication
    lastSignalCheckTime = millis();
    lastDiagTelemetryTime = millis();
    Serial.println("[Diag] Diagnostic mode STARTED — MONITOR_EN_PIN HIGH, LED active");
    server.send(200, "application/json", "{\"diagnostic\":\"started\",\"monitor_pin\":\"HIGH\",\"led\":\"enabled\"}");
}

void handleDiagStop() {
    diagnosticMode = false;
    // Restore voltage monitor pin to LOW (normal idle state)
    digitalWrite(MONITOR_EN_PIN, LOW);
    // Turn off the LED to save power
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    Serial.println("[Diag] Diagnostic mode STOPPED — MONITOR_EN_PIN LOW, LED off");
    server.send(200, "application/json", "{\"diagnostic\":\"stopped\",\"monitor_pin\":\"LOW\",\"led\":\"disabled\"}");
}

// ─── I2S Signal LED Indicator ────────────────────────────────────────────────
// Uses the onboard RGB LED (GPIO 48) to show audio pipeline health.
// Only active during diagnostic mode to save power.
// Error states are latched for a minimum duration so they're visible.
//   - RED:    Mic broken — >=50% of I2S frames empty, or >=50% Opus frames silent
//             (wiring fault: pin shorted to GND, intermittent connection, etc.)
//   - YELLOW: Degraded — 10-50% of frames failing (marginal connection)
//   - GREEN:  Healthy — mic active and Opus encoding real audio content
#define LED_LATCH_DURATION_MS 3000  // Hold error state for at least 3 seconds
unsigned long ledLatchUntil = 0;    // millis() timestamp when latch expires
uint8_t latchedState = 0;           // 0=green, 1=yellow, 2=red

void updateSignalLed() {
    if (!diagnosticMode) return;  // LED stays off outside diagnostic mode

    uint32_t total = i2sFramesTotal;
    uint32_t empty = i2sFramesEmpty;
    uint32_t opTotal = opusFramesEncoded;
    uint32_t opSilent = opusFramesSilent;

    // Determine worst status between I2S and Opus levels
    bool i2sDead = (total == 0 || empty == total);
    bool opusDead = (opTotal > 0 && opSilent == opTotal);
    bool opusIntermittent = (opTotal > 0 && opSilent > 0 && !opusDead);

    // Classify I2S empty frame ratio into severity tiers:
    //   - >=50% empty: mic is effectively dead (red) — wiring fault, shorted pin
    //   - >=10% empty: intermittent connection (yellow) — occasional bad contact
    //   - <10% empty:  healthy (green) — rare glitches are normal
    bool i2sMostlyDead = (total > 0 && empty * 2 >= total);       // >=50% empty → red
    bool i2sIntermittent = (total > 0 && empty > 0 && !i2sMostlyDead
                           && empty * 10 >= total);                // >=10% empty → yellow

    // Similarly for Opus: if most frames encode as silence, the mic is useless
    bool opusMostlySilent = (opTotal > 0 && opSilent * 2 >= opTotal);  // >=50% silent → red

    // Determine current state
    uint8_t currentState = 0;  // green
    if (i2sDead || opusDead || i2sMostlyDead || opusMostlySilent) {
        currentState = 2;  // red
    } else if (i2sIntermittent || opusIntermittent) {
        currentState = 1;  // yellow
    }

    // Latch: upgrade severity immediately, or refresh timer at same severity.
    // Never downgrade while the latch timer is active.
    if (currentState > latchedState) {
        // Worse state detected — latch it
        latchedState = currentState;
        ledLatchUntil = millis() + LED_LATCH_DURATION_MS;
    } else if (currentState == latchedState && currentState > 0) {
        // Same error level persists — keep the latch alive
        ledLatchUntil = millis() + LED_LATCH_DURATION_MS;
    }

    // If latch has expired, allow return to the current (possibly better) state
    if (millis() >= ledLatchUntil) {
        latchedState = currentState;
    }

    // Apply LED color based on latched state
    switch (latchedState) {
        case 2:  rgbLedWrite(RGB_BUILTIN, 20, 0, 0);   break;  // Red
        case 1:  rgbLedWrite(RGB_BUILTIN, 20, 15, 0);  break;  // Yellow
        default: rgbLedWrite(RGB_BUILTIN, 0, 20, 0);   break;  // Green
    }

    // Reset counters for the next window
    i2sFramesTotal = 0;
    i2sFramesEmpty = 0;
    opusFramesEncoded = 0;
    opusFramesSilent = 0;
}

// ─── I2S Setup ───────────────────────────────────────────────────────────────
void i2sInit() {
    // Power up the INMP441 microphone from a GPIO pin
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(100);  // Allow mic to stabilize after power-on

    // Create RX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Channel creation failed: %d\n", err);
        return;
    }

    // Configure standard mode for INMP441 (Philips/I2S format, 32-bit, mono left)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SCK_PIN,
            .ws = (gpio_num_t)I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // INMP441 outputs on the left channel when WS is low
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Std mode init failed: %d\n", err);
        return;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Channel enable failed: %d\n", err);
        return;
    }

    Serial.println("[I2S] Initialized (new driver)");
}

// ─── Opus Encoder Setup ──────────────────────────────────────────────────────
void opusInit() {
    opusFrameSamples = sampleRate * OPUS_FRAME_MS / 1000;

    // Input buffer in internal RAM — Opus encoder does random-access reads on this,
    // and PSRAM can return stale data under cache pressure, causing silent gaps.
    // At 48kHz/60ms this is only 5760 bytes — fits easily in internal SRAM.
    opusInputBuffer = (int16_t*)malloc(sizeof(int16_t) * opusFrameSamples);
    // Output and packet buffers in internal RAM for fast random access during encoding/framing
    opusOutputBuffer = (uint8_t*)malloc(OPUS_MAX_FRAME_SIZE);
    udpPacketBuffer = (uint8_t*)malloc(UDP_PACKET_BUF_SIZE);
    // Burst buffer in PSRAM — holds completed packets until burst send
    burstBuffer = (BurstPacket*)ps_malloc(sizeof(BurstPacket) * BURST_MAX_PACKETS);

    if (!opusInputBuffer || !opusOutputBuffer || !udpPacketBuffer || !burstBuffer) {
        Serial.println("[Opus] ERROR: Failed to allocate buffers!");
        return;
    }

    opusInputIndex = 0;
    udpPacketOffset = UDP_PACKET_HEADER_SIZE;  // reserve header space
    udpPacketFrameCount = 0;
    udpSequenceNumber = 0;
    burstWriteIdx = 0;
    burstReadIdx = 0;
    lastBurstTime = millis();

    int error;
    opusEncoder = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !opusEncoder) {
        Serial.printf("[Opus] ERROR: Failed to create encoder (error %d)\n", error);
        return;
    }

    opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(OPUS_BITRATE_BPS));
    opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY_LEVEL));
    opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    // Enable DTX (discontinuous transmission) — saves bandwidth during silence
    opus_encoder_ctl(opusEncoder, OPUS_SET_DTX(1));

    Serial.printf("[Opus] Encoder initialized — %d Hz, %d ms frames (%d samples), %d bps\n",
        sampleRate, OPUS_FRAME_MS, opusFrameSamples, OPUS_BITRATE_BPS);
    Serial.printf("[Opus] Bundling %d frames/packet, burst every %d ms (%d ms buffered)\n",
        OPUS_FRAMES_PER_PACKET, BURST_INTERVAL_MS,
        OPUS_FRAME_MS * OPUS_FRAMES_PER_PACKET * BURST_MAX_PACKETS);
}

// ─── OTA Setup ───────────────────────────────────────────────────────────────
void otaInit() {
    ArduinoOTA.setHostname("esp32-birdnet");

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("[OTA] Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] Update complete");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] Ready");
}

// ─── WiFi Setup via WiFiManager ──────────────────────────────────────────────

// Attempt a single WiFi connection with the given timeout (ms).
// Returns true if connected.
bool wifiTryConnect(const char* ssid, const char* password, unsigned long timeoutMs) {
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

// Hard-reset the WiFi radio — clears stuck state from deep sleep/brownout
void wifiRadioReset() {
    WiFi.disconnect(true, false);  // disconnect + clear STA config, keep NVS creds
    WiFi.mode(WIFI_OFF);
    delay(500);                    // let the radio fully power down
    WiFi.mode(WIFI_STA);
    delay(200);
}

void wifiInit() {
    Serial.println("[WiFi] Initializing...");

    // ─── Step 1: Fully power-cycle the radio ─────────────────────────────────
    // The ESP32-S3 WiFi radio often fails to associate on first boot after
    // deep sleep or power-on. A full OFF→STA cycle forces the RF calibration
    // to run fresh. We do NOT call WiFi.disconnect(true) here because that
    // erases the stored STA config that WiFiManager saved last time.
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    delay(200);

    // Set hostname BEFORE WiFi.begin() — this is what the DHCP client sends
    // to the router so the device shows up with a human-readable name.
    WiFi.setHostname(MDNS_HOSTNAME);
    Serial.printf("[WiFi] Hostname set to: %s\n", MDNS_HOSTNAME);

    // Disable WiFi modem sleep — keeps the radio active during connection,
    // preventing DTIM-related association timeouts on some routers.
    WiFi.setSleep(false);

    // Set WiFi TX power to max for reliable connection at distance
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // Print MAC address early — useful for identifying the device on the router
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());

    // ─── Step 2: Try connecting with stored credentials (fast path) ──────────
    // Before involving WiFiManager, try a direct WiFi.begin() which uses the
    // credentials stored in ESP32 NVS by a previous successful connection.
    // This avoids WiFiManager's overhead and is the path that usually works
    // on the 2nd/3rd reboot — we just need to give it enough time.
    Serial.println("[WiFi] Trying stored credentials (fast path)...");

    WiFi.begin();  // No args = use NVS-stored SSID/password

    // Wait up to 15 seconds for association + DHCP
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] Connected via stored credentials (fast path)");
    }

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    // Compile-time credentials provided — try if stored creds didn't work
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WiFi] Trying build-time credentials '%s'...\n", WIFI_SSID);

        const int maxAttempts = 3;
        const unsigned long timeouts[] = { 15000, 20000, 30000 };

        for (int attempt = 0; attempt < maxAttempts; attempt++) {
            Serial.printf("[WiFi] Attempt %d/%d (timeout %lums)...\n",
                attempt + 1, maxAttempts, timeouts[attempt]);

            if (wifiTryConnect(WIFI_SSID, WIFI_PASSWORD, timeouts[attempt])) {
                break;
            }

            Serial.printf("[WiFi] Attempt %d failed (status=%d)\n", attempt + 1, WiFi.status());

            if (attempt < maxAttempts - 1) {
                Serial.println("[WiFi] Resetting radio before retry...");
                wifiRadioReset();
                WiFi.setHostname(MDNS_HOSTNAME);
                WiFi.setSleep(false);
                delay(1000);
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Build-time credentials exhausted");
            wifiRadioReset();
            WiFi.setHostname(MDNS_HOSTNAME);
            delay(500);
        }
    }
#endif

    // ─── Step 3: WiFiManager (portal fallback) ───────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Starting WiFiManager...");

        // Reset radio cleanly before WiFiManager takes over
        wifiRadioReset();
        WiFi.setHostname(MDNS_HOSTNAME);
        WiFi.setSleep(false);
        delay(500);

        WiFiManager wm;
        wm.setHostname(MDNS_HOSTNAME);

        // Key reliability settings for WiFiManager:
        wm.setConnectTimeout(30);     // 30s per attempt (default is too short for some routers)
        wm.setConnectRetries(5);      // 5 retries with saved creds before opening portal
        wm.setCleanConnect(true);     // disconnect before each attempt (fixes stuck radio state)
        wm.setSaveConnect(true);      // persist successful connection in NVS
        wm.setWiFiAutoReconnect(true); // enable ESP32 auto-reconnect after WiFiManager exits

        // Debug output helps diagnose connection issues in serial monitor
        wm.setDebugOutput(true, "WM: ");

        WiFiManagerParameter customUdpHost("udp_host", "UDP Host", udpHost, sizeof(udpHost));
        WiFiManagerParameter customUdpPort("udp_port", "UDP Port", udpPort, sizeof(udpPort));
        WiFiManagerParameter customSampleRate("sample_rate", "Sample Rate (8000-48000)", sampleRateStr, sizeof(sampleRateStr));
        WiFiManagerParameter customLat("latitude", "Latitude", latitude, sizeof(latitude));
        WiFiManagerParameter customLon("longitude", "Longitude", longitude, sizeof(longitude));
        WiFiManagerParameter customUtc("utc_offset", "UTC Offset (hours)", utcOffset, sizeof(utcOffset));

        wm.addParameter(&customUdpHost);
        wm.addParameter(&customUdpPort);
        wm.addParameter(&customSampleRate);
        wm.addParameter(&customLat);
        wm.addParameter(&customLon);
        wm.addParameter(&customUtc);
        wm.setSaveParamsCallback(saveParamsCallback);

        wm.setConfigPortalTimeout(180);
        wm.setConnectTimeout(20);  // Allow 20 seconds for WiFi connection

        bool connected = wm.autoConnect("BirdNet-Setup");

        if (!connected) {
            Serial.println("[WiFi] All connection methods failed — restarting in 5s");
            delay(5000);
            ESP.restart();
        }

        // Only overwrite NVS values if WiFiManager portal was actually used
        if (strlen(customUdpHost.getValue()) > 0) {
            strncpy(udpHost, customUdpHost.getValue(), sizeof(udpHost) - 1);
        }
        if (strlen(customUdpPort.getValue()) > 0) {
            strncpy(udpPort, customUdpPort.getValue(), sizeof(udpPort) - 1);
        }
        if (strlen(customSampleRate.getValue()) > 0) {
            uint32_t newRate = atoi(customSampleRate.getValue());
            if (newRate == 8000 || newRate == 16000 || newRate == 22050 ||
                newRate == 32000 || newRate == 44100 || newRate == 48000) {
                sampleRate = newRate;
                snprintf(sampleRateStr, sizeof(sampleRateStr), "%lu", (unsigned long)sampleRate);
            }
        }
        if (strlen(customLat.getValue()) > 0) {
            strncpy(latitude, customLat.getValue(), sizeof(latitude) - 1);
        }
        if (strlen(customLon.getValue()) > 0) {
            strncpy(longitude, customLon.getValue(), sizeof(longitude) - 1);
        }
        if (strlen(customUtc.getValue()) > 0) {
            strncpy(utcOffset, customUtc.getValue(), sizeof(utcOffset) - 1);
        }

        saveSettings();
    }

    // ─── Connection established — print full network details ─────────────────
    // Enable ESP32 auto-reconnect — if the radio drops, it will automatically
    // try to reconnect without needing our loop() watchdog to notice first.
    WiFi.setAutoReconnect(true);

    Serial.println("[WiFi] ╔══════════════════════════════════════════════╗");
    Serial.println("[WiFi] ║         CONNECTED SUCCESSFULLY              ║");
    Serial.println("[WiFi] ╚══════════════════════════════════════════════╝");
    Serial.printf("[WiFi]   SSID       : %s\n", WiFi.SSID().c_str());
    Serial.printf("[WiFi]   IP Address : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi]   Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("[WiFi]   Gateway    : %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("[WiFi]   DNS        : %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("[WiFi]   MAC Address: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WiFi]   Hostname   : %s\n", WiFi.getHostname());
    Serial.printf("[WiFi]   RSSI       : %d dBm\n", WiFi.RSSI());
    Serial.printf("[WiFi]   Channel    : %d\n", WiFi.channel());
    Serial.printf("[WiFi]   TX Power   : %d\n", WiFi.getTxPower());
    Serial.println("[WiFi] ──────────────────────────────────────────────");
    Serial.printf("[Config] UDP Target   : %s:%s\n", udpHost, udpPort);
    Serial.printf("[Config] Sample Rate  : %lu Hz\n", (unsigned long)sampleRate);
    Serial.printf("[Config] Location     : Lat %s, Lon %s, UTC%s\n", latitude, longitude, utcOffset);
    Serial.printf("[Config] Sleep Enabled: %s\n", sleepEnabled ? "yes" : "no");
    Serial.printf("[Config] Debug Mode  : %s\n", debugMode ? "yes (sleep disabled)" : "no");
    Serial.println("[WiFi] ──────────────────────────────────────────────");
}

// ─── NTP Time Sync ───────────────────────────────────────────────────────────
void syncTime() {
    int offset = atoi(utcOffset);

    // Build a POSIX TZ string that includes DST (North American rules:
    // DST starts 2nd Sunday of March, ends 1st Sunday of November).
    // Standard offset is inverted in POSIX (UTC-5 becomes EST5EDT4).
    // This ensures summer times are correct without requiring user to
    // manually adjust the offset for DST.
    char tzStr[64];
    int absOffset = abs(offset);
    int dstOffset = absOffset - 1;  // DST is 1 hour ahead (smaller UTC offset)

    // POSIX TZ sign is inverted: west of UTC is positive in POSIX
    int posixStdOffset = -offset;
    int posixDstOffset = posixStdOffset - 1;

    snprintf(tzStr, sizeof(tzStr), "STD%dDST%d,M3.2.0/2,M11.1.0/2",
             posixStdOffset, posixDstOffset);

    Serial.printf("[NTP] Timezone: %s (UTC%+d with DST)\n", tzStr, offset);
    configTzTime(tzStr, NTP_SERVER);

    Serial.print("[NTP] Waiting for time sync");
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 20) {
        Serial.print(".");
        delay(500);
        retries++;
    }
    Serial.println();

    if (retries >= 20) {
        Serial.println("[NTP] Failed to sync time — restarting");
        delay(3000);
        ESP.restart();
    }

    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d (DST: %s)\n",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
        timeinfo.tm_isdst > 0 ? "yes" : "no");
}

// ─── Sunrise/Sunset Calculation ──────────────────────────────────────────────
void getSunTimes(int year, int month, int day, double &sunriseMin, double &sunsetMin) {
    double lat = atof(latitude);
    double lon = atof(longitude);
    int offset = atoi(utcOffset);

    // If DST is currently active (set by configTzTime), adjust the offset
    struct tm timeinfo;
    if (getLocalTime(&timeinfo) && timeinfo.tm_isdst > 0) {
        offset += 1;  // DST: effective offset is 1 hour closer to UTC
    }

    SunSet sun;
    sun.setPosition(lat, lon, offset);
    sun.setCurrentDate(year, month, day);

    sunriseMin = sun.calcSunrise();
    sunsetMin  = sun.calcSunset();
}

// ─── Check if current time is within active window ───────────────────────────
// Active window: (sunrise - 60 minutes) to (sunset + 60 minutes)
bool isWithinActiveWindow() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[Schedule] Cannot get local time");
        return true; // Default to active if time unknown
    }

    double sunriseMin, sunsetMin;
    getSunTimes(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                sunriseMin, sunsetMin);

    // Validate sunrise/sunset values — the SunSet library can return
    // negative values or values > 1440 for extreme latitudes or when
    // the UTC offset doesn't match DST.  Clamp to sane bounds.
    if (sunriseMin < 0 || sunriseMin > 1440 || sunsetMin < 0 || sunsetMin > 1440) {
        Serial.printf("[Schedule] WARNING: Invalid sun times (sunrise=%.1f, sunset=%.1f) — defaulting to active\n",
            sunriseMin, sunsetMin);
        return true;
    }

    // Sanity check: sunrise must come before sunset
    if (sunriseMin >= sunsetMin) {
        Serial.printf("[Schedule] WARNING: sunrise (%.1f) >= sunset (%.1f) — defaulting to active\n",
            sunriseMin, sunsetMin);
        return true;
    }

    double activeStart = sunriseMin - 60.0; // 1 hour before sunrise
    double activeEnd   = sunsetMin + 60.0;  // 1 hour after sunset

    // Clamp activeStart to midnight — don't wrap into the previous day
    if (activeStart < 0.0) {
        activeStart = 0.0;
    }
    // Clamp activeEnd to end of day
    if (activeEnd > 1440.0) {
        activeEnd = 1440.0;
    }

    double nowMin = timeinfo.tm_hour * 60.0 + timeinfo.tm_min + timeinfo.tm_sec / 60.0;

    Serial.printf("[Schedule] Now: %.1f min | Active: %.1f – %.1f min (sunrise=%.1f, sunset=%.1f)\n",
        nowMin, activeStart, activeEnd, sunriseMin, sunsetMin);

    return (nowMin >= activeStart && nowMin <= activeEnd);
}

// ─── Calculate seconds until next active window ──────────────────────────────
uint64_t secondsUntilNextActiveWindow() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[Schedule] Cannot get time — fallback 1 hour sleep");
        return 3600; // Fallback: sleep 1 hour and retry
    }

    double nowMin = timeinfo.tm_hour * 60.0 + timeinfo.tm_min + timeinfo.tm_sec / 60.0;

    // Calculate tomorrow's sunrise
    time_t now = mktime(&timeinfo);
    now += 86400;
    struct tm tomorrow;
    localtime_r(&now, &tomorrow);

    double sunriseMin, sunsetMin;
    getSunTimes(tomorrow.tm_year + 1900, tomorrow.tm_mon + 1, tomorrow.tm_mday,
                sunriseMin, sunsetMin);

    // Validate tomorrow's sun times
    if (sunriseMin < 0 || sunriseMin > 1440 || sunsetMin < 0 || sunsetMin > 1440) {
        Serial.printf("[Schedule] Invalid tomorrow sun times (sunrise=%.1f, sunset=%.1f) — fallback 1 hour\n",
            sunriseMin, sunsetMin);
        return 3600;
    }

    double tomorrowActiveStart = sunriseMin - 60.0;
    if (tomorrowActiveStart < 0.0) {
        tomorrowActiveStart = 0.0;
    }

    double minutesUntilTomorrow = (24.0 * 60.0) - nowMin;
    double totalMinutes = minutesUntilTomorrow + tomorrowActiveStart;

    // Sanity: should never be negative or more than ~24 hours away
    if (totalMinutes <= 0.0 || totalMinutes > 24.0 * 60.0) {
        Serial.printf("[Schedule] Calculated sleep %.1f min out of range — fallback 1 hour\n", totalMinutes);
        return 3600;
    }

    uint64_t seconds = (uint64_t)(totalMinutes * 60.0);
    if (seconds < 60) seconds = 60;

    Serial.printf("[Schedule] Sleeping for %llu seconds (%.1f hours) until tomorrow's active window\n",
        seconds, seconds / 3600.0);

    return seconds;
}

// ─── Deep Sleep ──────────────────────────────────────────────────────────────
// Maximum single sleep duration: 2 hours. If we need to sleep longer, the device
// will wake, quickly re-evaluate the schedule, and go back to sleep. This avoids
// RTC timer overflow/drift issues on long sleep durations.
#define MAX_SLEEP_SECONDS  (2ULL * 3600)

void enterDeepSleep(uint64_t sleepSeconds) {
    // Debug mode: refuse to sleep under any circumstances
    if (debugMode) {
        Serial.printf("[Sleep] BLOCKED — debug mode active (would have slept %llu s)\n", sleepSeconds);
        return;
    }

    // Cap sleep duration to avoid RTC timer reliability issues
    if (sleepSeconds > MAX_SLEEP_SECONDS) {
        Serial.printf("[Sleep] Requested %llu s — capping to %llu s (will re-check on wake)\n",
            sleepSeconds, MAX_SLEEP_SECONDS);
        sleepSeconds = MAX_SLEEP_SECONDS;
    }

    // Guard against zero or absurdly small values
    if (sleepSeconds < 60) {
        sleepSeconds = 60;
    }

    Serial.printf("[Sleep] Entering deep sleep for %llu seconds (%.1f hours)\n",
        sleepSeconds, sleepSeconds / 3600.0);

    // Ensure microphone is powered down before sleeping
    digitalWrite(MIC_POWER_PIN, LOW);

    Serial.flush();
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ─── Audio Streaming ─────────────────────────────────────────────────────────
#define RESOLVE_INTERVAL_MS  (60 * 1000)  // re-resolve every 60s

void udpInit() {
    // Bind the audio UDP socket to a local port (0 = system picks an ephemeral port).
    // This MUST be called before beginPacket/endPacket or sends will fail with ENOMEM.
    if (udp.begin(0)) {
        Serial.println("[UDP] Audio socket bound successfully");
    } else {
        Serial.println("[UDP] ERROR: Failed to bind audio socket!");
    }

    Serial.printf("[UDP] Target: %s:%s\n", udpHost, udpPort);
    Serial.printf("[UDP] Retry: %d attempts, %dms delay | Backoff: %d-%dms\n",
        UDP_RETRY_ATTEMPTS, UDP_RETRY_DELAY_MS, UDP_BACKOFF_BASE_MS, UDP_BACKOFF_MAX_MS);

    // Resolve target immediately so first burst doesn't fail
    resolvedUdpAddr = resolveHost(udpHost);
    lastResolveTime = millis();
    if (resolvedUdpAddr != IPAddress(0, 0, 0, 0)) {
        Serial.printf("[UDP] Resolved target: %s -> %s\n", udpHost, resolvedUdpAddr.toString().c_str());
    } else {
        Serial.printf("[UDP] WARNING: Cannot resolve %s — will retry later\n", udpHost);
    }

    lastUdpStatsTime = millis();
}

void streamAudio() {
    size_t bytesRead = 0;

    esp_err_t err = i2s_channel_read(rx_handle, i2sReadBuffer, sizeof(i2sReadBuffer), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK || bytesRead == 0) {
        return;
    }

    // Convert 32-bit I2S samples to 16-bit: MEMS mics output 24-bit data
    // MSB-aligned in the 32-bit word, shift right to extract meaningful bits
    int samplesRead32 = bytesRead / sizeof(int32_t);
    for (int i = 0; i < samplesRead32; i++) {
        i2sBuffer[i] = (int16_t)(i2sReadBuffer[i] >> 14);
    }

    // Signal quality checks — only run in diagnostic mode to avoid
    // wasting CPU cycles during normal streaming operation.
    if (diagnosticMode) {
        i2sFramesTotal = i2sFramesTotal + 1;
        
        // Check raw 32-bit amplitude — real mic signal produces large magnitudes.
        // A properly connected INMP441 drives the bus on most samples, not just a few.
        // Intermittent wiring produces occasional spikes but fails the majority test.
        int activeCount = 0;
        const int32_t RAW_SIGNAL_THRESHOLD = 100000;
        for (int i = 0; i < samplesRead32; i++) {
            int32_t raw = i2sReadBuffer[i];
            if (raw > RAW_SIGNAL_THRESHOLD || raw < -RAW_SIGNAL_THRESHOLD) {
                activeCount++;
            }
        }
        
        // Require at least 10% of samples above threshold.
        // An intermittent SD pin might produce sporadic spikes from noise coupling
        // that pass a 5% threshold but fail at 10%.
        if (activeCount <= samplesRead32 / 10) {
            // Mic not driving bus (unpowered, SD shorted to GND, intermittent, etc.)
            i2sFramesEmpty = i2sFramesEmpty + 1;
        } else {
            // Bus appears driven — perform structural checks on the 16-bit audio.
            // These detect hardware faults (stuck lines, shorted pins) without
            // false-triggering on legitimate quiet audio.
            int16_t minVal = i2sBuffer[0], maxVal = i2sBuffer[0];
            int64_t sum = 0;
            for (int i = 0; i < samplesRead32; i++) {
                if (i2sBuffer[i] < minVal) minVal = i2sBuffer[i];
                if (i2sBuffer[i] > maxVal) maxVal = i2sBuffer[i];
                sum += i2sBuffer[i];
            }
            int peakToPeak = (int)maxVal - (int)minVal;
            int16_t dcOffset = (int16_t)(sum / samplesRead32);
            
            // Stuck sample detection (data line fault — SCK or SD not toggling)
            int maxStuckRun = 0, currentStuckRun = 1;
            for (int i = 1; i < samplesRead32; i++) {
                if (i2sBuffer[i] == i2sBuffer[i - 1]) {
                    currentStuckRun++;
                    if (currentStuckRun > maxStuckRun) maxStuckRun = currentStuckRun;
                } else {
                    currentStuckRun = 1;
                }
            }
            
            // Fault conditions (any one is sufficient to mark frame empty):
            //
            // 1. Stuck line: >25% consecutive identical samples means the data
            //    line is not transitioning. Normal audio never produces this.
            bool stuckLine = (maxStuckRun > samplesRead32 / 4);
            
            // 2. DC rail: large DC offset means SD is pulled to VDD or GND.
            //    INMP441 in I2S mode outputs near-zero DC. A bias above ±5000
            //    in the 16-bit domain (after >>14) indicates a shorted pin.
            bool badDcBias = (dcOffset > 5000 || dcOffset < -5000);
            
            // 3. Completely flat: peak-to-peak == 0 means every sample is identical
            //    (different from stuck runs — this catches the entire buffer being
            //    one value, e.g. all zeros from an unpowered mic that passed the
            //    raw threshold due to noise on the bus lines).
            bool totallyFlat = (peakToPeak == 0);
            
            // 4. Uncorrelated noise: a floating or shorted SD pin produces random
            //    noise with near-zero lag-1 autocorrelation. Real audio at 48kHz
            //    is heavily band-limited and has strong sample-to-sample correlation
            //    (typically >0.5, often >0.9). Random bus noise is <0.1.
            //    This is the key test for "sounds blank but has energy."
            int64_t crossSum = 0;
            int64_t autoSum = 0;
            for (int i = 1; i < samplesRead32; i++) {
                crossSum += (int64_t)i2sBuffer[i] * (int64_t)i2sBuffer[i - 1];
                autoSum  += (int64_t)i2sBuffer[i] * (int64_t)i2sBuffer[i];
            }
            // correlation < 0.3 means the signal lacks temporal structure
            // (using 0.3 instead of 0.1 to catch partially-connected pins that
            // produce some correlated content mixed with noise)
            bool uncorrelatedNoise = (autoSum > 0 && crossSum * 10 < autoSum * 3);
            
            if (stuckLine || badDcBias || totallyFlat || uncorrelatedNoise) {
                i2sFramesEmpty = i2sFramesEmpty + 1;
            }
        }
    }

    // Re-resolve the UDP target periodically
    if (resolvedUdpAddr == IPAddress(0, 0, 0, 0) ||
        millis() - lastResolveTime >= RESOLVE_INTERVAL_MS) {
        resolvedUdpAddr = resolveHost(udpHost);
        lastResolveTime = millis();
        if (resolvedUdpAddr != IPAddress(0, 0, 0, 0)) {
            Serial.printf("[UDP] Resolved target: %s -> %s\n", udpHost, resolvedUdpAddr.toString().c_str());
        }
    }

    if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) {
        return; // Can't send if host is unresolved
    }

    if (!opusEncoder) {
        return; // Encoder not initialized
    }

    uint16_t port = (uint16_t)atoi(udpPort);
    int samplesRead = samplesRead32;

    // Feed PCM samples into Opus frame accumulator and encode when full
    int offset = 0;
    while (offset < samplesRead) {
        int remaining = opusFrameSamples - opusInputIndex;
        int available = samplesRead - offset;
        int toCopy = min(remaining, available);

        memcpy(&opusInputBuffer[opusInputIndex], &i2sBuffer[offset], toCopy * sizeof(int16_t));
        opusInputIndex += toCopy;
        offset += toCopy;

        // When we've accumulated a full Opus frame, encode it
        if (opusInputIndex >= opusFrameSamples) {
            int encodedBytes = opus_encode(opusEncoder, opusInputBuffer, opusFrameSamples,
                                           opusOutputBuffer, OPUS_MAX_FRAME_SIZE);
            opusInputIndex = 0;

            if (encodedBytes <= 0) {
                // Opus encoding error — skip this frame
                continue;
            }

            // Track Opus output size for diagnostic LED — very small frames
            // indicate the encoder received silence/blank input
            if (diagnosticMode) {
                opusFramesEncoded = opusFramesEncoded + 1;
                if (encodedBytes <= OPUS_SILENT_THRESHOLD) {
                    opusFramesSilent = opusFramesSilent + 1;
                }
            }

            // Check if this frame fits in the current packet buffer
            int needed = 2 + encodedBytes;
            if (udpPacketOffset + needed > UDP_PACKET_BUF_SIZE) {
                // Flush current packet first, then start a new one with this frame
                flushUdpPacket(port);
            }

            // Append encoded frame: [2-byte big-endian length][frame data]
            udpPacketBuffer[udpPacketOffset++] = (uint8_t)(encodedBytes >> 8);
            udpPacketBuffer[udpPacketOffset++] = (uint8_t)(encodedBytes & 0xFF);
            memcpy(&udpPacketBuffer[udpPacketOffset], opusOutputBuffer, encodedBytes);
            udpPacketOffset += encodedBytes;
            udpPacketFrameCount++;

            // Flush when we've accumulated the target number of frames
            if (udpPacketFrameCount >= OPUS_FRAMES_PER_PACKET) {
                flushUdpPacket(port);
            }
        }
    }
}

// Queue the assembled multi-frame packet into the burst buffer
void flushUdpPacket(uint16_t port) {
    if (udpPacketFrameCount == 0) return;

    // Write header: magic bytes + frame count + sequence number
    udpPacketBuffer[0] = 0x4F;  // 'O'
    udpPacketBuffer[1] = 0x50;  // 'P'
    udpPacketBuffer[2] = (uint8_t)udpPacketFrameCount;
    udpPacketBuffer[3] = udpSequenceNumber++;

    // Queue into burst ring buffer
    int nextWrite = (burstWriteIdx + 1) % BURST_MAX_PACKETS;
    if (nextWrite == burstReadIdx) {
        // Buffer full — force an immediate burst to drain
        burstSendPackets();

        // If still full after burst attempt (sends are failing), drop oldest packet
        nextWrite = (burstWriteIdx + 1) % BURST_MAX_PACKETS;
        if (nextWrite == burstReadIdx) {
            udpPacketsDropped++;
            burstReadIdx = (burstReadIdx + 1) % BURST_MAX_PACKETS;
            if (udpPacketsDropped <= 5 || udpPacketsDropped % 50 == 0) {
                Serial.printf("[UDP] Ring buffer overflow — dropped packet #%lu\n", udpPacketsDropped);
            }
        }
    }

    memcpy(burstBuffer[burstWriteIdx].data, udpPacketBuffer, udpPacketOffset);
    burstBuffer[burstWriteIdx].length = udpPacketOffset;
    burstWriteIdx = (burstWriteIdx + 1) % BURST_MAX_PACKETS;

    // Reset for next packet
    udpPacketOffset = UDP_PACKET_HEADER_SIZE;
    udpPacketFrameCount = 0;
}

// Log periodic UDP performance stats for diagnostics
void logUdpStats() {
    unsigned long total = udpSendSuccess + udpSendErrors;
    if (total == 0) return;

    float successRate = (udpSendSuccess * 100.0f) / total;
    int queued = (burstWriteIdx - burstReadIdx + BURST_MAX_PACKETS) % BURST_MAX_PACKETS;

    Serial.printf("[UDP:Stats] sent=%lu fail=%lu (%.1f%% success) | "
                  "dropped=%lu bursts=%lu | "
                  "queue=%d/%d | backoff=%lums | RSSI=%d\n",
        udpSendSuccess, udpSendErrors, successRate,
        udpPacketsDropped, udpBurstCount,
        queued, BURST_MAX_PACKETS - 1,
        udpBackoffMs, WiFi.RSSI());

    if (udpConsecutiveFails > 0) {
        Serial.printf("[UDP:Stats] Currently in failure state: %lu consecutive fails\n",
            udpConsecutiveFails);
    }

    lastUdpStatsTime = millis();
}

// Burst-send all queued packets at once — minimizes WiFi radio active time
// If telemetry is pending, it's appended as a trailer to the last packet in the burst.
void burstSendPackets() {
    if (burstReadIdx == burstWriteIdx) return;  // nothing queued

    if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) {
        // Try to re-resolve immediately rather than waiting for the periodic timer
        resolvedUdpAddr = resolveHost(udpHost);
        lastResolveTime = millis();
        if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) {
            return;
        }
        Serial.printf("[UDP] Re-resolved target: %s -> %s\n", udpHost, resolvedUdpAddr.toString().c_str());
    }

    // Check WiFi is actually connected before attempting to send
    if (WiFi.status() != WL_CONNECTED) {
        // Don't burn send attempts when WiFi is down — just skip this burst
        if (udpConsecutiveFails == 0) {
            Serial.println("[UDP] WiFi disconnected — skipping burst");
        }
        udpConsecutiveFails++;
        return;
    }

    // Respect dynamic backoff after repeated failures
    if (udpBackoffMs > 0 && (millis() - lastBurstFailTime) < udpBackoffMs) {
        return;  // Still in backoff period
    }

    uint16_t port = (uint16_t)atoi(udpPort);
    int packetsSent = 0;
    int packetsFailed = 0;

    // Determine how many packets are queued so we can identify the last one
    int queuedCount = (burstWriteIdx - burstReadIdx + BURST_MAX_PACKETS) % BURST_MAX_PACKETS;
    int packetIndex = 0;

    // Consume the telemetry flag atomically — only attach once per pending cycle
    bool attachTelemetry = telemetryPending;
    if (attachTelemetry) {
        telemetryPending = false;
    }

    while (burstReadIdx != burstWriteIdx) {
        BurstPacket &pkt = burstBuffer[burstReadIdx];
        bool isLastPacket = (packetIndex == queuedCount - 1);
        bool sent = false;

        // Retry loop for each packet
        for (int attempt = 0; attempt < UDP_RETRY_ATTEMPTS; attempt++) {
            if (!udp.beginPacket(resolvedUdpAddr, port)) {
                if (attempt < UDP_RETRY_ATTEMPTS - 1) {
                    delay(UDP_RETRY_DELAY_MS);
                    continue;
                }
                break;  // All retries exhausted for beginPacket
            }
            udp.write(pkt.data, pkt.length);

            // Append telemetry trailer to the last packet in the burst
            if (isLastPacket && attachTelemetry) {
                udp.write(telemetryTrailer, TELEMETRY_TRAILER_SIZE);
            }

            if (udp.endPacket()) {
                sent = true;
                break;
            }

            // endPacket failed — retry after brief delay
            if (attempt < UDP_RETRY_ATTEMPTS - 1) {
                delay(UDP_RETRY_DELAY_MS);
            }
        }

        if (sent) {
            packetsSent++;
            udpSendSuccess++;
            if (!udpFirstPacketLogged) {
                Serial.printf("[UDP] First burst sent (%d bytes to %s:%d)\n",
                    pkt.length, resolvedUdpAddr.toString().c_str(), port);
                udpFirstPacketLogged = true;
            }
        } else {
            packetsFailed++;
            udpSendErrors++;

            // Log failures with context — include WiFi RSSI for correlation
            if (udpSendErrors <= 5 || udpSendErrors % 100 == 0) {
                Serial.printf("[UDP] Send failed #%lu (RSSI: %d dBm, heap: %lu, pkt_len: %d)\n",
                    udpSendErrors, WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(), pkt.length);
            }

            // Stop this burst on failure — remaining packets stay queued
            // If telemetry wasn't sent yet, re-flag it for the next burst
            if (isLastPacket && attachTelemetry) {
                telemetryPending = true;
            } else if (attachTelemetry && !isLastPacket) {
                // We failed mid-burst; telemetry was intended for last packet.
                // Re-flag so next burst attempt will attach it.
                telemetryPending = true;
            }
            break;
        }

        burstReadIdx = (burstReadIdx + 1) % BURST_MAX_PACKETS;
        packetIndex++;
        yield();  // Let lwIP process between packets in the burst
    }

    // Update backoff state based on burst outcome
    if (packetsFailed > 0) {
        udpConsecutiveFails++;
        lastBurstFailTime = millis();

        // Exponential backoff: double each time, capped at max
        if (udpBackoffMs == 0) {
            udpBackoffMs = UDP_BACKOFF_BASE_MS;
        } else {
            udpBackoffMs = min(udpBackoffMs * 2, (unsigned long)UDP_BACKOFF_MAX_MS);
        }

        // If we've been failing a lot, try re-resolving the target
        if (udpConsecutiveFails == 10 || udpConsecutiveFails == 50) {
            Serial.printf("[UDP] %lu consecutive failures — re-resolving target\n", udpConsecutiveFails);
            resolvedUdpAddr = resolveHost(udpHost);
            lastResolveTime = millis();
            if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) {
                Serial.printf("[UDP] Cannot resolve %s — target unreachable\n", udpHost);
            } else {
                Serial.printf("[UDP] Re-resolved: %s -> %s\n", udpHost, resolvedUdpAddr.toString().c_str());
            }
        }
    } else if (packetsSent > 0) {
        // Success — reset backoff state
        if (udpConsecutiveFails > 0) {
            Serial.printf("[UDP] Recovered after %lu consecutive failures (backoff was %lums)\n",
                udpConsecutiveFails, udpBackoffMs);
        }
        udpConsecutiveFails = 0;
        udpBackoffMs = 0;
    }

    udpBurstCount++;
    lastBurstTime = millis();
}

// ─── Main ────────────────────────────────────────────────────────────────────
#define SCHEDULE_CHECK_INTERVAL_MS  (5 * 60 * 1000)
#define MDNS_ANNOUNCE_INTERVAL_MS   (120 * 1000)  // Re-advertise mDNS every 2 minutes
#define WIFI_CHECK_INTERVAL_MS      (30 * 1000)   // Check WiFi connectivity every 30s
#define WIFI_RECONNECT_MAX_ATTEMPTS 5             // Max reconnect attempts before reboot
unsigned long lastScheduleCheck = 0;
unsigned long lastMdnsAnnounce = 0;
unsigned long lastWifiCheck = 0;
int wifiReconnectAttempts = 0;
bool streamingStarted = false;
TaskHandle_t audioTaskHandle = nullptr;

// ─── Audio Task (runs on dedicated core with large stack) ────────────────────
void audioTask(void *param) {
    Serial.println("[Audio] Task started on core " + String(xPortGetCoreID()));
    for (;;) {
        streamAudio();

        // Check if it's time to burst-send queued packets
        if (millis() - lastBurstTime >= BURST_INTERVAL_MS) {
            burstSendPackets();
        }
    }
}

void startStreaming() {
    if (streamingStarted) return;

    Serial.println("[Boot] Configuration received — starting audio streaming");
    syncTime();

    // Check schedule — go to sleep if outside window and sleep is enabled
    if (sleepEnabled && !isWithinActiveWindow()) {
        Serial.println("[Schedule] Outside active window — going to sleep");
        uint64_t sleepSec = secondsUntilNextActiveWindow();
        enterDeepSleep(sleepSec);
        // Never reaches here
    }

    Serial.println("[Schedule] Within active window — starting audio services");
    i2sInit();
    opusInit();
    udpInit();

    // Launch audio streaming on a dedicated task with 32KB stack
    // Pinned to core 1 to avoid contention with WiFi (core 0)
    xTaskCreatePinnedToCore(
        audioTask,          // task function
        "audio_stream",     // name
        32768,              // stack size (bytes) — Opus encoder needs deep stack
        NULL,               // parameter
        5,                  // priority (above normal)
        &audioTaskHandle,   // handle
        1                   // core 1
    );

    streamingStarted = true;
    lastScheduleCheck = millis();
    Serial.println("──────────────────────────────────────────────────");
    Serial.println("[Boot] Streaming audio");
    Serial.printf("[Boot] Free Heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.println("══════════════════════════════════════════════════\n");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Turn off the onboard RGB LED immediately — it may retain state across resets
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);

    Serial.println("\n══════════════════════════════════════════════════");
    Serial.println("       ESP32 BirdNet Streamer — Booting");
    Serial.println("══════════════════════════════════════════════════");
    Serial.printf("[Boot] CPU Freq     : %d MHz\n", getCpuFrequencyMhz());
    Serial.printf("[Boot] Free Heap    : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("[Boot] Flash Size   : %lu bytes\n", (unsigned long)ESP.getFlashChipSize());
    Serial.printf("[Boot] SDK Version  : %s\n", ESP.getSdkVersion());
    Serial.printf("[Boot] Chip Model   : %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[Boot] Reset Reason : %d\n", (int)esp_reset_reason());
    Serial.println("──────────────────────────────────────────────────");

    // Load saved settings from NVS before WiFi (so portal shows current values)
    loadSettings();

    wifiInit();

    // Always start OTA, mDNS, and HTTP so the device is discoverable
    otaInit();
    mdnsInit();
    httpInit();
    powerMonitorInit();
    lastMdnsAnnounce = millis();

    if (configured) {
        // Device was previously configured — go straight to streaming
        startStreaming();
    } else {
        // Not yet configured — wait in discovery mode
        Serial.println("══════════════════════════════════════════════════");
        Serial.println("[Boot] WAITING FOR CONFIGURATION");
        Serial.printf("[Boot] Connect to http://%s.local/config to configure\n", MDNS_HOSTNAME);
        Serial.printf("[Boot] Or POST to http://%s/config\n", WiFi.localIP().toString().c_str());
        Serial.println("══════════════════════════════════════════════════");
    }
}

void loop() {
    ArduinoOTA.handle();
    server.handleClient();

    // ─── WiFi watchdog: detect disconnection and auto-reconnect ──────────
    if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            wifiReconnectAttempts++;
            Serial.printf("[WiFi] Disconnected! Reconnect attempt %d/%d...\n",
                wifiReconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);

            WiFi.disconnect(false);
            delay(100);
            WiFi.begin();  // reconnect using stored credentials

            // Wait up to 10s for reconnection
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
                delay(250);
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[WiFi] Reconnected! IP: %s, RSSI: %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
                wifiReconnectAttempts = 0;
            } else if (wifiReconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
                Serial.println("[WiFi] Too many reconnect failures — rebooting");
                delay(1000);
                ESP.restart();
            }
        } else {
            wifiReconnectAttempts = 0;  // reset counter when connected
        }
    }

    // If we just got configured, transition to streaming
    if (configured && !streamingStarted) {
        startStreaming();
    }

    // Periodically re-advertise mDNS so restarted listeners can discover us
    if (millis() - lastMdnsAnnounce >= MDNS_ANNOUNCE_INTERVAL_MS) {
        lastMdnsAnnounce = millis();
        // Query our own hostname — this triggers mDNS traffic that keeps
        // our records fresh in the caches of other machines on the LAN
        MDNS.queryHost(MDNS_HOSTNAME, 1000);
    }

    // Periodically check if we've passed sunset
    if (streamingStarted) {
        if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL_MS) {
            lastScheduleCheck = millis();

            if (sleepEnabled && !isWithinActiveWindow()) {
                Serial.println("[Schedule] Active window ended — going to sleep");
                uint64_t sleepSec = secondsUntilNextActiveWindow();
                enterDeepSleep(sleepSec);
            }
        }

        // Periodically read battery/solar voltages and send telemetry
        if (millis() - lastPowerReadTime >= POWER_READ_INTERVAL_MS) {
            lastPowerReadTime = millis();
            readPowerMonitor();
            sendTelemetryPacket();
        }

        // Periodically log UDP performance stats
        if (millis() - lastUdpStatsTime >= UDP_STATS_INTERVAL_MS) {
            logUdpStats();
        }

        // Update onboard LED to reflect I2S signal status
        if (millis() - lastSignalCheckTime >= SIGNAL_CHECK_INTERVAL_MS) {
            lastSignalCheckTime = millis();
            updateSignalLed();
        }

        // In diagnostic mode, send telemetry every 10 seconds
        if (diagnosticMode && millis() - lastDiagTelemetryTime >= DIAG_TELEMETRY_INTERVAL_MS) {
            lastDiagTelemetryTime = millis();
            readPowerMonitor();
            sendTelemetryPacket();
        }
    }

    delay(10); // Yield time — audio runs on its own task
}
