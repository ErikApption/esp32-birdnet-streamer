#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <time.h>
#include <sunset.h>
#include <esp_system.h>
#include <opus.h>

// Onboard LED
#define RGB_BUILTIN 48

// ─── I2S Configuration ───────────────────────────────────────────────────────
#define I2S_WS_PIN        5   // Word Select (LRCLK)
#define I2S_SD_PIN        6   // Serial Data (DOUT from mic)
#define I2S_SCK_PIN       4   // Serial Clock (BCLK)
#define I2S_PORT          I2S_NUM_0

// ─── Microphone Power Pin ────────────────────────────────────────────────────
#define MIC_POWER_PIN     10  // GPIO powering INMP441 VDD (~1.4 mA)

#define DEFAULT_SAMPLE_RATE 48000
#define SAMPLE_BITS       I2S_BITS_PER_SAMPLE_32BIT
#define I2S_CHANNEL_FMT   I2S_CHANNEL_FMT_ONLY_LEFT
#define DMA_BUF_COUNT     8
#define DMA_BUF_LEN       1024

// ─── UDP Configuration ───────────────────────────────────────────────────────
#define DEFAULT_UDP_HOST  "192.168.1.100"
#define DEFAULT_UDP_PORT  4000

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

// Telemetry packet: magic "TL" + battery_adc_mV (uint16) + solar_adc_mV (uint16)
#define TELEMETRY_MAGIC_0   0x54  // 'T'
#define TELEMETRY_MAGIC_1   0x4C  // 'L'
#define TELEMETRY_PACKET_SIZE 6   // 2 magic + 2 bat_adc_mV + 2 sol_adc_mV

// ─── HTTP Config Server ──────────────────────────────────────────────────────
#define HTTP_PORT         80

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiUDP udp;
WebServer server(HTTP_PORT);
Preferences prefs;

char udpHost[64]    = DEFAULT_UDP_HOST;
char udpPort[6]     = "4000";
char latitude[12]   = DEFAULT_LATITUDE;
char longitude[12]  = DEFAULT_LONGITUDE;
char utcOffset[4]   = DEFAULT_UTC_OFFSET;
bool sleepEnabled   = false;

// ─── Diagnostic Mode State ────────────────────────────────────────────────────
bool diagnosticMode = false;  // true when /diag/start has been called
unsigned long lastDiagTelemetryTime = 0;
#define DIAG_TELEMETRY_INTERVAL_MS 2000  // Send telemetry every 2s in diagnostic mode

// ─── I2S Signal Monitor State ────────────────────────────────────────────────
volatile bool i2sSignalDetected = false;  // set by audio task when samples are non-zero
unsigned long lastSignalCheckTime = 0;
#define SIGNAL_CHECK_INTERVAL_MS 500  // LED update interval

// ─── Power Monitor State ─────────────────────────────────────────────────────
float lastBatteryVoltage = 0.0f;  // raw ADC voltage (before divider scaling)
float lastSolarVoltage   = 0.0f;  // raw ADC voltage (before divider scaling)
unsigned long lastPowerReadTime = 0;

// ─── UDP Target (used by streaming and telemetry) ────────────────────────────
IPAddress resolvedUdpAddr;
unsigned long lastResolveTime = 0;
unsigned long udpSendErrors = 0;
bool udpFirstPacketLogged = false;

uint32_t sampleRate = DEFAULT_SAMPLE_RATE;
char sampleRateStr[8] = "48000";
bool configured     = false;  // true once a config has been received (persisted in NVS)

static int32_t i2sReadBuffer[DMA_BUF_LEN];  // 32-bit I2S read buffer
static int16_t i2sBuffer[DMA_BUF_LEN];     // 16-bit samples after bit-shift conversion

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
#define UDP_PACKET_BUF_SIZE 1400     // stay well under MTU (1500 - 20 IP - 8 UDP = 1472)

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

    if (changed) {
        configured = true;
        saveSettings();
        Serial.printf("[HTTP] Config updated — UDP: %s:%s, rate: %lu, sleep: %s\n",
            udpHost, udpPort, (unsigned long)sampleRate, sleepEnabled ? "on" : "off");
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
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"battery_v\":" + String(lastBatteryVoltage, 3) + ",";
    json += "\"solar_v\":" + String(lastSolarVoltage, 3) + ",";
    json += "\"diagnostic\":" + String(diagnosticMode ? "true" : "false");
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
    server.on("/diag/start", HTTP_POST, handleDiagStart);
    server.on("/diag/stop", HTTP_POST, handleDiagStop);
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
    delay(2);  // RC settling time

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

    uint16_t port = (uint16_t)atoi(udpPort);
    // Send raw ADC millivolts (before divider scaling)
    uint16_t batMv = (uint16_t)(lastBatteryVoltage * 1000.0f);
    uint16_t solMv = (uint16_t)(lastSolarVoltage * 1000.0f);

    uint8_t pkt[TELEMETRY_PACKET_SIZE];
    pkt[0] = TELEMETRY_MAGIC_0;       // 'T'
    pkt[1] = TELEMETRY_MAGIC_1;       // 'L'
    pkt[2] = (uint8_t)(batMv >> 8);   // battery ADC mV big-endian
    pkt[3] = (uint8_t)(batMv & 0xFF);
    pkt[4] = (uint8_t)(solMv >> 8);   // solar ADC mV big-endian
    pkt[5] = (uint8_t)(solMv & 0xFF);

    udp.beginPacket(resolvedUdpAddr, port);
    udp.write(pkt, TELEMETRY_PACKET_SIZE);
    udp.endPacket();
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
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);
    Serial.println("[Diag] Diagnostic mode STOPPED — MONITOR_EN_PIN LOW, LED off");
    server.send(200, "application/json", "{\"diagnostic\":\"stopped\",\"monitor_pin\":\"LOW\",\"led\":\"disabled\"}");
}

// ─── I2S Signal LED Indicator ────────────────────────────────────────────────
// Uses the onboard RGB LED (GPIO 48) to show I2S microphone signal status.
// Only active during diagnostic mode to save power.
//   - RED:   No signal detected (all zeros / silence)
//   - GREEN: Signal is being received
void updateSignalLed() {
    if (!diagnosticMode) return;  // LED stays off outside diagnostic mode

    if (i2sSignalDetected) {
        neopixelWrite(RGB_BUILTIN, 0, 20, 0);  // Green — signal OK
    } else {
        neopixelWrite(RGB_BUILTIN, 20, 0, 0);  // Red — no signal
    }
    // Reset the flag; the audio task will set it again if signal is present
    i2sSignalDetected = false;
}

// ─── I2S Setup ───────────────────────────────────────────────────────────────
void i2sInit() {
    // Power up the INMP441 microphone from a GPIO pin
    pinMode(MIC_POWER_PIN, OUTPUT);
    digitalWrite(MIC_POWER_PIN, HIGH);
    delay(100);  // Allow mic to stabilize after power-on

    i2s_config_t i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = sampleRate,
        .bits_per_sample = SAMPLE_BITS,
        .channel_format = I2S_CHANNEL_FMT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pinConfig = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Driver install failed: %d\n", err);
    }

    err = i2s_set_pin(I2S_PORT, &pinConfig);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Set pin failed: %d\n", err);
    }

    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("[I2S] Initialized");
}

// ─── Opus Encoder Setup ──────────────────────────────────────────────────────
void opusInit() {
    opusFrameSamples = sampleRate * OPUS_FRAME_MS / 1000;

    // Input buffer in PSRAM (large, sequential writes only)
    opusInputBuffer = (int16_t*)ps_malloc(sizeof(int16_t) * opusFrameSamples);
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
void wifiInit() {
    Serial.println("[WiFi] Initializing...");

    // Set hostname BEFORE WiFi.mode() and WiFi.begin() — this is what the
    // DHCP client sends to the router so the device shows up with a
    // human-readable name in the router's client list.
    WiFi.setHostname(MDNS_HOSTNAME);
    Serial.printf("[WiFi] Hostname set to: %s\n", MDNS_HOSTNAME);

    // Print MAC address early — useful for identifying the device on the router
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    // Compile-time credentials provided — try connecting directly first
    Serial.printf("[WiFi] Connecting to '%s' (build-time credentials)...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WiFi] Build-time credentials failed (status=%d) — falling back to captive portal\n",
            WiFi.status());
        WiFi.disconnect(true);
        delay(100);
    }
#endif

    // If not yet connected (no build-time creds, or they failed), use WiFiManager
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Starting WiFiManager captive portal...");
        WiFiManager wm;

        // Set WiFiManager hostname so it also propagates during portal mode
        wm.setHostname(MDNS_HOSTNAME);

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

        bool connected = wm.autoConnect("BirdNet-Setup");

        if (!connected) {
            Serial.println("[WiFi] Failed to connect — restarting in 3s");
            delay(3000);
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
    // Bind the UDP socket to a local port (0 = system picks an ephemeral port).
    // This MUST be called before beginPacket/endPacket or sends will fail with ENOMEM.
    if (udp.begin(0)) {
        Serial.println("[UDP] Socket bound successfully");
    } else {
        Serial.println("[UDP] ERROR: Failed to bind socket!");
    }
    Serial.printf("[UDP] Target: %s:%s\n", udpHost, udpPort);
}

void streamAudio() {
    size_t bytesRead = 0;

    esp_err_t err = i2s_read(I2S_PORT, i2sReadBuffer, sizeof(i2sReadBuffer), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK || bytesRead == 0) {
        return;
    }

    // Convert 32-bit I2S samples to 16-bit: MEMS mics output 24-bit data
    // MSB-aligned in the 32-bit word, shift right to extract meaningful bits
    int samplesRead32 = bytesRead / sizeof(int32_t);
    for (int i = 0; i < samplesRead32; i++) {
        i2sBuffer[i] = (int16_t)(i2sReadBuffer[i] >> 14);
    }

    // Signal detection: check if any samples exceed a noise floor threshold
    // This drives the onboard LED indicator (red = no signal, green = signal)
    for (int i = 0; i < samplesRead32; i++) {
        if (abs(i2sBuffer[i]) > 16) {  // threshold above ADC noise floor
            i2sSignalDetected = true;
            break;
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
    }

    memcpy(burstBuffer[burstWriteIdx].data, udpPacketBuffer, udpPacketOffset);
    burstBuffer[burstWriteIdx].length = udpPacketOffset;
    burstWriteIdx = (burstWriteIdx + 1) % BURST_MAX_PACKETS;

    // Reset for next packet
    udpPacketOffset = UDP_PACKET_HEADER_SIZE;
    udpPacketFrameCount = 0;
}

// Burst-send all queued packets at once — minimizes WiFi radio active time
void burstSendPackets() {
    if (burstReadIdx == burstWriteIdx) return;  // nothing queued

    if (resolvedUdpAddr == IPAddress(0, 0, 0, 0)) return;

    uint16_t port = (uint16_t)atoi(udpPort);
    int packetsSent = 0;

    while (burstReadIdx != burstWriteIdx) {
        BurstPacket &pkt = burstBuffer[burstReadIdx];

        if (!udp.beginPacket(resolvedUdpAddr, port)) {
            delay(1);
            break;  // Socket not ready — try again next burst
        }
        udp.write(pkt.data, pkt.length);
        int sent = udp.endPacket();

        if (sent) {
            packetsSent++;
            if (!udpFirstPacketLogged) {
                Serial.printf("[UDP] First burst sent (%d bytes to %s:%d)\n",
                    pkt.length, resolvedUdpAddr.toString().c_str(), port);
                udpFirstPacketLogged = true;
            }
        } else {
            udpSendErrors++;
            if (udpSendErrors == 1 || udpSendErrors == 100 || udpSendErrors == 1000 ||
                udpSendErrors % 10000 == 0) {
                Serial.printf("[UDP] Send failed (total errors: %lu)\n", udpSendErrors);
            }
            break;  // Back off on failure
        }

        burstReadIdx = (burstReadIdx + 1) % BURST_MAX_PACKETS;
        yield();  // Let lwIP process between packets in the burst
    }

    lastBurstTime = millis();
}

// ─── Main ────────────────────────────────────────────────────────────────────
#define SCHEDULE_CHECK_INTERVAL_MS  (5 * 60 * 1000)
#define MDNS_ANNOUNCE_INTERVAL_MS   (120 * 1000)  // Re-advertise mDNS every 2 minutes
unsigned long lastScheduleCheck = 0;
unsigned long lastMdnsAnnounce = 0;
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
