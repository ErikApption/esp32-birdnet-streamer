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

// ─── I2S Configuration ───────────────────────────────────────────────────────
#define I2S_WS_PIN        5   // Word Select (LRCLK)
#define I2S_SD_PIN        6   // Serial Data (DOUT from mic)
#define I2S_SCK_PIN       4   // Serial Clock (BCLK)
#define I2S_PORT          I2S_NUM_0

#define DEFAULT_SAMPLE_RATE 48000
#define SAMPLE_BITS       I2S_BITS_PER_SAMPLE_16BIT
#define I2S_CHANNEL_FMT   I2S_CHANNEL_FMT_ONLY_LEFT
#define DMA_BUF_COUNT     4
#define DMA_BUF_LEN       1024

// ─── UDP Configuration ───────────────────────────────────────────────────────
#define DEFAULT_UDP_HOST  "192.168.1.100"
#define DEFAULT_UDP_PORT  4000

// ─── Location defaults (used for sunrise/sunset calculation) ─────────────────
#define DEFAULT_LATITUDE   "45.4215"
#define DEFAULT_LONGITUDE  "-75.6972"
#define DEFAULT_UTC_OFFSET "-5"

// ─── NTP Configuration ──────────────────────────────────────────────────────
#define NTP_SERVER        "pool.ntp.org"

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
uint32_t sampleRate = DEFAULT_SAMPLE_RATE;
char sampleRateStr[8] = "48000";
bool configured     = false;  // true once a config has been received (persisted in NVS)

static int16_t i2sBuffer[DMA_BUF_LEN];

// ─── mDNS Hostname ───────────────────────────────────────────────────────────
#define MDNS_HOSTNAME     "esp32-birdnet"
#define MDNS_SERVICE      "birdnet"

// ─── Forward declarations ────────────────────────────────────────────────────
void wifiInit();
void otaInit();
void i2sInit();
void udpInit();
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
    json += "\"rssi\":" + String(WiFi.RSSI());
    json += "}";
    server.send(200, "application/json", json);
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

void httpInit() {
    server.on("/config", HTTP_GET, handleGetConfig);
    server.on("/config", HTTP_POST, handlePostConfig);
    server.on("/status", HTTP_GET, handleGetStatus);
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

// ─── I2S Setup ───────────────────────────────────────────────────────────────
void i2sInit() {
    i2s_config_t i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = sampleRate,
        .bits_per_sample = SAMPLE_BITS,
        .channel_format = I2S_CHANNEL_FMT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
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
    configTime(offset * 3600, 0, NTP_SERVER);

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

    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

// ─── Sunrise/Sunset Calculation ──────────────────────────────────────────────
void getSunTimes(int year, int month, int day, double &sunriseMin, double &sunsetMin) {
    double lat = atof(latitude);
    double lon = atof(longitude);
    int offset = atoi(utcOffset);

    SunSet sun;
    sun.setPosition(lat, lon, offset);
    sun.setCurrentDate(year, month, day);

    sunriseMin = sun.calcSunrise();
    sunsetMin  = sun.calcSunset();
}

// ─── Check if current time is within active window ───────────────────────────
// Active window: (sunrise - 60 minutes) to sunset
bool isWithinActiveWindow() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("[Schedule] Cannot get local time");
        return true; // Default to active if time unknown
    }

    double sunriseMin, sunsetMin;
    getSunTimes(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                sunriseMin, sunsetMin);

    double activeStart = sunriseMin - 60.0; // 1 hour before sunrise
    double activeEnd   = sunsetMin;

    double nowMin = timeinfo.tm_hour * 60.0 + timeinfo.tm_min + timeinfo.tm_sec / 60.0;

    Serial.printf("[Schedule] Now: %.1f min | Active: %.1f – %.1f min (sunrise=%.1f, sunset=%.1f)\n",
        nowMin, activeStart, activeEnd, sunriseMin, sunsetMin);

    return (nowMin >= activeStart && nowMin <= activeEnd);
}

// ─── Calculate seconds until next active window ──────────────────────────────
uint64_t secondsUntilNextActiveWindow() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
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

    double tomorrowActiveStart = sunriseMin - 60.0;

    double minutesUntilTomorrow = (24.0 * 60.0) - nowMin;
    double totalMinutes = minutesUntilTomorrow + tomorrowActiveStart;

    uint64_t seconds = (uint64_t)(totalMinutes * 60.0);
    if (seconds < 60) seconds = 60;

    Serial.printf("[Schedule] Sleeping for %llu seconds (%.1f hours) until tomorrow's active window\n",
        seconds, seconds / 3600.0);

    return seconds;
}

// ─── Deep Sleep ──────────────────────────────────────────────────────────────
void enterDeepSleep(uint64_t sleepSeconds) {
    Serial.printf("[Sleep] Entering deep sleep for %llu seconds\n", sleepSeconds);
    Serial.flush();

    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

// ─── Audio Streaming ─────────────────────────────────────────────────────────
// Cached resolved UDP target IP (re-resolved periodically or on config change)
IPAddress resolvedUdpAddr;
unsigned long lastResolveTime = 0;
unsigned long udpSendErrors = 0;
bool udpFirstPacketLogged = false;
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

    esp_err_t err = i2s_read(I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK || bytesRead == 0) {
        return;
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

    uint16_t port = (uint16_t)atoi(udpPort);

    // Send the entire I2S buffer in one UDP packet (max ~1460 bytes safe for
    // Ethernet MTU). DMA_BUF_LEN=1024 samples × 2 bytes = 2048 bytes, which
    // exceeds typical MTU, so we split into chunks that fit without IP fragmentation.
    const size_t maxPacket = 1440;  // safe for standard 1500 MTU minus headers
    size_t offset = 0;

    while (offset < bytesRead) {
        size_t chunkSize = min(maxPacket, bytesRead - offset);

        if (!udp.beginPacket(resolvedUdpAddr, port)) {
            // Socket not ready — back off briefly and skip this cycle
            delay(1);
            return;
        }
        udp.write((uint8_t*)i2sBuffer + offset, chunkSize);
        int sent = udp.endPacket();

        if (sent) {
            if (!udpFirstPacketLogged) {
                Serial.printf("[UDP] First packet sent OK (%u bytes to %s:%d)\n",
                    chunkSize, resolvedUdpAddr.toString().c_str(), port);
                udpFirstPacketLogged = true;
            }
        } else {
            udpSendErrors++;
            // Only log occasionally to avoid flooding serial
            if (udpSendErrors == 1 || udpSendErrors == 100 || udpSendErrors == 1000 ||
                udpSendErrors % 10000 == 0) {
                Serial.printf("[UDP] Send failed (total errors: %lu) — buffer pressure\n",
                    udpSendErrors);
            }
            // Back off to let the network stack drain its buffers
            delay(2);
            return;  // Drop remaining chunks this cycle, I2S will refill next call
        }

        offset += chunkSize;

        // Small yield between chunks so lwIP can process TX completions
        if (offset < bytesRead) {
            yield();
        }
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────
#define SCHEDULE_CHECK_INTERVAL_MS  (5 * 60 * 1000)
unsigned long lastScheduleCheck = 0;
bool streamingStarted = false;

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
    udpInit();

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

    // Only stream audio if fully configured and started
    if (streamingStarted) {
        streamAudio();

        // Periodically check if we've passed sunset
        if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL_MS) {
            lastScheduleCheck = millis();

            if (sleepEnabled && !isWithinActiveWindow()) {
                Serial.println("[Schedule] Active window ended — going to sleep");
                uint64_t sleepSec = secondsUntilNextActiveWindow();
                enterDeepSleep(sleepSec);
            }
        }
    }
}
