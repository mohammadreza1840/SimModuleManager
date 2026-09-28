#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include "panel_config.h"
#include "listen_page.h"

/* =========================================================================
 * TTGO T-Call (ESP32 + SIM800L) SMS Gateway & Call Streamer
 * - Local WebServer for Sending SMS, Contacts, Health
 * - Webhook Forwarding for Incoming SMS
 * - SIM Hot-Swap Support
 * - ADC GPIO34 to PCM16LE Live Voice Streamer (Port 8080)
 * ========================================================================= */

// --- TTGO T-Call Hardware Pinout ---
#define MODEM_RST            5
#define MODEM_PWKEY          4
#define MODEM_POWER_ON       23
#define MODEM_TX             27
#define MODEM_RX             26
#define I2C_SDA              21
#define I2C_SCL              22

// Audio input: conditioned analog signal on GPIO34 (ADC1_CH6).
// SIM800 PCM pins are not used by this firmware.
#define I2S_NUM              I2S_NUM_0

// LilyGO SIM800L IP5306 20200811 exposes SPK_2P/N (auxiliary channel).
// Override with -DMODEM_AUDIO_CHANNEL=0 only for wiring to the main channel.
#ifndef MODEM_AUDIO_CHANNEL
#define MODEM_AUDIO_CHANNEL 1
#endif
static_assert(MODEM_AUDIO_CHANNEL == 0 || MODEM_AUDIO_CHANNEL == 1,
              "This firmware captures analog audio, not SIM800 PCM");

#define IP5306_ADDR          0x75
#define IP5306_REG_SYS_CTL0  0x00

#define SerialMon Serial
HardwareSerial SerialAT(1);

// --- Network & Config ---
const char* WIFI_SSID       = CONFIG_WIFI_SSID;
const char* WIFI_PASS       = CONFIG_WIFI_PASS;

// --- Static IP Configuration ---
// (Commented out for DHCP)
// IPAddress local_IP(CONFIG_STATIC_IP);
// IPAddress gateway(CONFIG_STATIC_GATEWAY);
// IPAddress subnet(CONFIG_STATIC_SUBNET);
// IPAddress primaryDNS(CONFIG_STATIC_DNS1);   // Optional
// IPAddress secondaryDNS(CONFIG_STATIC_DNS2); // Optional

// --- Central Panel Config ---
const char* PANEL_URL       = PANEL_BASE_URL;
String boardDeviceId;
String boardDeviceToken;
String currentPhone;
bool panelRegistered = false;
unsigned long lastRegistrationAttempt = 0;
unsigned long registrationRetryMs = 0;
String registeredIP;
String bootId;

static String randomId(const char* prefix) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    char value[48];
    int used = snprintf(value, sizeof(value), "%s-", prefix);
    for (size_t i = 0; i < sizeof(bytes); ++i)
        used += snprintf(value + used, sizeof(value) - used, "%02x", bytes[i]);
    return String(value);
}

// A board identity survives SIM swaps; a separate random credential survives reboots.
bool initPanelIdentity() {
    uint64_t mac = ESP.getEfuseMac();
    char id[32];
    snprintf(id, sizeof(id), "esp32-%04x%08x",
             (unsigned)((mac >> 32) & 0xffff), (unsigned)(mac & 0xffffffff));
    boardDeviceId = id;
    Preferences preferences;
    if (!preferences.begin("panel-auth", false)) return false;
    boardDeviceToken = preferences.getString("device-token", "");
    if (boardDeviceToken.isEmpty()) {
        uint8_t randomBytes[32];
        esp_fill_random(randomBytes, sizeof(randomBytes));
        char token[65];
        for (size_t i = 0; i < sizeof(randomBytes); ++i)
            snprintf(token + i * 2, 3, "%02x", randomBytes[i]);
        boardDeviceToken = token;
        if (preferences.putString("device-token", boardDeviceToken) != 64)
            boardDeviceToken = "";
    }
    preferences.end();
    return boardDeviceToken.length() == 64;
}

WebServer server(80);
WiFiServer audioServer(8080); // سرور اختصاصی استریم صدا روی پورت 8080

// --- State Machine ---
enum SimState {
    SIM_MISSING,
    SIM_INIT, 
    SIM_READY
};

SimState currentState = SIM_MISSING;
unsigned long lastSimCheckTime = 0;
int simCheckFailCount = 0;
String incomingSmsBuffer = "";
String currentICCID = ""; // شناسه یکتای سیمکارت فعلی

// --- Voice Recording State ---
bool isRecording = false;
bool audioReady = false;
bool audioRateVerified = false;
uint32_t measuredRawSampleRate = 0;
volatile uint64_t sampleCounter = 0;
portMUX_TYPE sampleCounterMux = portMUX_INITIALIZER_UNLOCKED;
bool modemAudioConfigured = false;
void audioStreamTask(void* parameter);

enum CallState { CALL_IDLE, CALL_RINGING, CALL_ANSWERING, CALL_ACTIVE, CALL_ENDING };
CallState callState = CALL_IDLE;
String currentCallId;
String callerNumber;
unsigned long lastClccPoll = 0;
unsigned long answeringStarted = 0;
bool callSeenInLastClcc = false;
uint8_t missingActiveClccPolls = 0;
bool stateSyncRequested = false;

struct CallEventMessage {
    char eventId[48];
    char callId[48];
    char type[12];
    char reason[20];
    char state[12];
    char caller[32];
    uint32_t sequence;
    uint64_t counter;
    uint32_t monotonicMs;
};
QueueHandle_t callEventQueue = nullptr;
uint32_t nextEventSequence = 1;

uint64_t getSampleCounter() {
    portENTER_CRITICAL(&sampleCounterMux);
    uint64_t value = sampleCounter;
    portEXIT_CRITICAL(&sampleCounterMux);
    return value;
}

String uint64String(uint64_t value) {
    char text[24];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    return String(text);
}

const char* callStateName() {
    switch (callState) {
        case CALL_RINGING: return "ringing";
        case CALL_ANSWERING: return "ringing";
        case CALL_ACTIVE: return "active";
        case CALL_ENDING: return "ended";
        default: return "idle";
    }
}

void queueCallEvent(const char* type, const char* reason = "", const char* state = "") {
    if (!callEventQueue) return;
    CallEventMessage event{};
    String id = randomId("event");
    strlcpy(event.eventId, id.c_str(), sizeof(event.eventId));
    strlcpy(event.callId, currentCallId.c_str(), sizeof(event.callId));
    strlcpy(event.type, type, sizeof(event.type));
    strlcpy(event.reason, reason, sizeof(event.reason));
    strlcpy(event.state, state, sizeof(event.state));
    strlcpy(event.caller, callerNumber.c_str(), sizeof(event.caller));
    event.sequence = nextEventSequence++;
    event.counter = getSampleCounter();
    event.monotonicMs = millis();
    if (xQueueSend(callEventQueue, &event, 0) != pdTRUE)
        SerialMon.println("[EVENT] Queue full; call metadata could not be queued.");
}

void sendCurrentStateEvent() {
    queueCallEvent("state", "", callStateName());
}

void finishCall(const char* reason) {
    if (callState == CALL_IDLE || callState == CALL_ENDING) return;
    callState = CALL_ENDING;
    queueCallEvent("ended", reason);
    SerialMon.printf("[CALL] Ended (%s); audio stream remains open.\n", reason);
    callState = CALL_IDLE;
    currentCallId = "";
    callerNumber = "";
    callSeenInLastClcc = false;
}

void callEventSenderTask(void*) {
    CallEventMessage event;
    for (;;) {
        if (xQueuePeek(callEventQueue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (WiFi.status() != WL_CONNECTED || !panelRegistered || boardDeviceToken.length() != 64) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        String base = PANEL_URL;
        while (base.endsWith("/")) base.remove(base.length() - 1);
        HTTPClient http;
        http.setConnectTimeout(2500);
        http.setTimeout(4000);
        int code = -1;
        if (http.begin(base + "/api/device-events/" + boardDeviceId + "/events")) {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + boardDeviceToken);
            StaticJsonDocument<640> doc;
            doc["boot_id"] = bootId;
            doc["event_id"] = event.eventId;
            doc["sequence"] = event.sequence;
            if (strlen(event.callId)) doc["call_id"] = event.callId;
            doc["type"] = event.type;
            if (strlen(event.reason)) doc["reason"] = event.reason;
            if (strlen(event.state)) doc["state"] = event.state;
            if (strlen(event.caller)) doc["caller_number"] = event.caller;
            char counter[24];
            snprintf(counter, sizeof(counter), "%llu", (unsigned long long)event.counter);
            doc["sample_counter"] = counter;
            doc["monotonic_ms"] = String(event.monotonicMs);
            String body;
            serializeJson(doc, body);
            code = http.POST(body);
            http.end();
        }
        if (code >= 200 && code < 300) {
            SerialMon.printf("[EVENT] Delivered %s seq=%u HTTP %d\n", event.type, event.sequence, code);
            xQueueReceive(callEventQueue, &event, 0);
        } else if (code == 400 || code == 401 || code == 403 || code == 404 || code == 409) {
            SerialMon.printf("[EVENT] Rejected %s seq=%u HTTP %d; dropping non-retryable event.\n",
                             event.type, event.sequence, code);
            xQueueReceive(callEventQueue, &event, 0);
        } else {
            SerialMon.printf("[EVENT] Delivery failed for seq=%u (%d); retrying same ID.\n", event.sequence, code);
            vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 1000)));
        }
    }
}

// Forward declarations
void handleSendSMS();
void handleGetContacts();
void processIncomingUARTLine(const String& line);
String sendAT(const String& cmd, unsigned long timeout = 2000, bool trimResponse = true);

/* =========================================================================
 * Hardware Initialization
 * ========================================================================= */

bool configureIP5306() {
    SerialMon.println("[HW] Configuring IP5306 PMU via I2C...");
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_SYS_CTL0);
    Wire.write(0x37); // Boost on, charge on, disable auto-sleep
    bool ok = (Wire.endTransmission() == 0);
    return ok;
}

void initModem() {
    configureIP5306();

    SerialMon.println("[SIM] Initializing Modem...");

    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);

    pinMode(MODEM_PWKEY, OUTPUT);
    digitalWrite(MODEM_PWKEY, HIGH);
    delay(100);
    digitalWrite(MODEM_PWKEY, LOW);
    delay(1000);
    digitalWrite(MODEM_PWKEY, HIGH);

    pinMode(MODEM_RST, OUTPUT);
    digitalWrite(MODEM_RST, HIGH);

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
}

void initI2S() {
    // ESP32's legacy built-in ADC/I2S path has a practical clock floor near
    // 44 kHz. Capture there, measure it for ten seconds, then decimate to the
    // declared telephony rate. The TCP writer never controls sample production.
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    
    esp_err_t err = i2s_driver_install(I2S_NUM, &i2s_config, 0, nullptr);
    if (err != ESP_OK) {
        SerialMon.printf("[AUDIO] Driver install failed: %d\n", err);
        return;
    }
    err = i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_6);
    if (err == ESP_OK) err = adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12);
    if (err == ESP_OK) err = i2s_adc_enable(I2S_NUM);
    if (err != ESP_OK) {
        SerialMon.printf("[AUDIO] ADC initialization failed: %d\n", err);
        i2s_driver_uninstall(I2S_NUM);
        return;
    }
    audioReady = true;
    SerialMon.printf("[AUDIO] ADC GPIO34 started; calibrating raw clock for 10 seconds before declaring %u Hz PCM.\n",
                     AUDIO_OUTPUT_SAMPLE_RATE);
    SerialMon.println("[AUDIO] External conditioned modem audio must be wired to GPIO34; ADC ready does not verify wiring.");
}

/* =========================================================================
 * AT Commands Helpers
 * ========================================================================= */

String sendAT(const String& cmd, unsigned long timeout, bool trimResponse) {
    SerialAT.println(cmd);
    String response = "";
    String line = "";
    unsigned long start = millis();
    while (millis() - start < timeout) {
        while (SerialAT.available()) {
            char c = (char)SerialAT.read();
            response += c;
            if (c == '\n') {
                line.trim();
                if (line.length()) processIncomingUARTLine(line);
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
        if (response.indexOf("\r\nOK\r\n") != -1 || response.indexOf("\r\nERROR\r\n") != -1) {
            break;
        }
        delay(10);
    }
    line.trim();
    if (line.length()) processIncomingUARTLine(line);
    if (trimResponse) response.trim();
    return response;
}

String getICCID() {
    String resp = sendAT("AT+CCID", 2000, true);
    String iccid = "";
    for (size_t i = 0; i < resp.length(); i++) {
        if (isDigit(resp.charAt(i))) {
            iccid += resp.charAt(i);
        }
    }
    return iccid;
}

/* =========================================================================
 * Encoding Helpers
 * ========================================================================= */

bool isAscii(const String& str) {
    for (size_t i = 0; i < str.length(); i++) {
        if ((unsigned char)str[i] > 127) return false;
    }
    return true;
}

String utf8ToUcs2Hex(const String& utf8) {
    String hexStr = "";
    for (size_t i = 0; i < utf8.length(); ) {
        uint8_t c = utf8[i];
        uint16_t cp = 0;
        if (c <= 0x7F) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
            i += 3;
        } else {
            i += 1; // Skip invalid
            continue;
        }
        char buf[5];
        sprintf(buf, "%04X", cp);
        hexStr += buf;
    }
    return hexStr;
}

bool isPureHexString(const String& str) {
    if (str.length() == 0 || str.length() % 4 != 0) return false;
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

String ucs2HexToUtf8(const String& hexStr) {
    String utf8 = "";
    for (size_t i = 0; i < hexStr.length(); i += 4) {
        String hexChar = hexStr.substring(i, i + 4);
        long cp = strtol(hexChar.c_str(), NULL, 16);
        if (cp <= 0x7F) {
            utf8 += (char)cp;
        } else if (cp <= 0x7FF) {
            utf8 += (char)(0xC0 | ((cp >> 6) & 0x1F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            utf8 += (char)(0xE0 | ((cp >> 12) & 0x0F));
            utf8 += (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        }
    }
    return utf8;
}

/* =========================================================================
 * Web Endpoints
 * ========================================================================= */

void handleSendSMS() {
    if (currentState != SIM_READY) {
        server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"SIM not ready\"}");
        return;
    }

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Body required\"}");
        return;
    }

    String body = server.arg("plain");
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err || !doc.containsKey("phone") || !doc.containsKey("message")) {
        server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Invalid JSON format. Requires 'phone' and 'message'\"}");
        return;
    }

    String phone = doc["phone"].as<String>();
    String message = doc["message"].as<String>();

    SerialMon.printf("[WEB] Request to send SMS to %s\n", phone.c_str());

    bool hasUnicode = !isAscii(message);

    // Switch to text mode
    sendAT("AT+CMGF=1", 1000, true);
    
    if (hasUnicode) {
        SerialMon.println("[WEB] Unicode detected. Using UCS2 encoding...");
        sendAT("AT+CSMP=17,167,0,8", 1000, true);
        sendAT("AT+CSCS=\"UCS2\"", 1000, true);
        
        SerialAT.print("AT+CMGS=\"");
        SerialAT.print(utf8ToUcs2Hex(phone));
        SerialAT.println("\"");
        
        delay(500); // Wait for '>'
        SerialAT.print(utf8ToUcs2Hex(message));
    } else {
        SerialMon.println("[WEB] ASCII text detected. Using GSM encoding...");
        sendAT("AT+CSMP=17,167,0,0", 1000, true);
        sendAT("AT+CSCS=\"GSM\"", 1000, true);
        
        SerialAT.print("AT+CMGS=\"");
        SerialAT.print(phone);
        SerialAT.println("\"");
        
        delay(500); // Wait for '>'
        SerialAT.print(message);
    }
    
    delay(100);
    SerialAT.write(26); // Ctrl+Z to send

    // Wait for response
    String resp = "";
    unsigned long start = millis();
    while(millis() - start < 10000) { // SMS sending can take a few seconds
        while(SerialAT.available()) {
            resp += (char)SerialAT.read();
        }
        if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) {
            break;
        }
        delay(10);
    }

    // Restore default GSM character set for incoming messages if it was changed
    if (hasUnicode) {
        sendAT("AT+CSCS=\"GSM\"", 1000, true);
    }

    if (resp.indexOf("OK") != -1) {
        server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"SMS Sent\"}");
    } else {
        server.send(500, "application/json", "{\"status\":\"error\", \"message\":\"Failed to send SMS\"}");
    }
}

void handleGetContacts() {
    if (currentState != SIM_READY) {
        server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"SIM not ready\"}");
        return;
    }

    SerialMon.println("[WEB] Request to fetch contacts...");

    // Set phonebook storage to SIM
    sendAT("AT+CPBS=\"SM\"", 2000, true);
    
    // Read all contacts (assuming max 250 on a standard SIM)
    String resp = sendAT("AT+CPBR=1,250", 10000, false);
    
    // Allocate on Heap instead of Stack to prevent Stack Overflow
    DynamicJsonDocument doc(16384); 
    JsonArray contacts = doc.createNestedArray("contacts");

    int lineStart = 0;
    while (lineStart < resp.length()) {
        int lineEnd = resp.indexOf('\n', lineStart);
        if (lineEnd == -1) lineEnd = resp.length();
        
        String line = resp.substring(lineStart, lineEnd);
        line.trim();
        
        // Sample response line: +CPBR: 1,"09123456789",129,"Ali"
        if (line.startsWith("+CPBR:")) {
            int firstComma = line.indexOf(',');
            int secondComma = line.indexOf(',', firstComma + 1);
            int thirdComma = line.indexOf(',', secondComma + 1);
            
            if (firstComma != -1 && secondComma != -1 && thirdComma != -1) {
                String number = line.substring(firstComma + 1, secondComma);
                String name = line.substring(thirdComma + 1);
                
                number.replace("\"", ""); // Remove quotes
                name.replace("\"", "");   // Remove quotes
                
                JsonObject contact = contacts.createNestedObject();
                contact["name"] = name;
                contact["phone"] = number;
            }
        }
        lineStart = lineEnd + 1;
    }

    String responseBody;
    serializeJson(doc, responseBody);
    server.send(200, "application/json", responseBody);
}

void handleHealthCheck() {
    String cpin = sendAT("AT+CPIN?", 1500, true);
    bool network = (cpin.indexOf("READY") != -1);
    String stateStr = (currentState == SIM_READY) ? "ready" : "missing";

    StaticJsonDocument<1280> doc;
    doc["status"] = "ok";
    doc["sim_state"] = stateStr;
    doc["network"] = network;
    doc["iccid"] = currentICCID;
    doc["ip"] = WiFi.localIP().toString();
    doc["audio_ready"] = audioReady;
    doc["device_id"] = boardDeviceId;
    doc["boot_id"] = bootId;
    doc["panel_registered"] = panelRegistered;
    doc["call_events_version"] = 1;
    doc["call_state"] = callStateName();
    if (currentCallId.length()) doc["current_call_id"] = currentCallId;
    else doc["current_call_id"] = nullptr;
    if (callerNumber.length()) doc["caller_number"] = callerNumber;
    else doc["caller_number"] = nullptr;
    doc["audio_source"] = "external_analog_gpio34";
    doc["audio_format"] = "pcm_s16le";
    doc["audio_channels"] = 1;
    if (audioRateVerified) doc["sample_rate"] = AUDIO_OUTPUT_SAMPLE_RATE;
    else doc["sample_rate"] = nullptr;
    doc["raw_sample_rate_measured"] = measuredRawSampleRate;
    doc["sample_counter"] = uint64String(getSampleCounter());
    doc["audio_wiring_verified"] = false; // Software cannot establish physical connectivity.
    doc["modem_audio_channel"] = MODEM_AUDIO_CHANNEL;
    doc["modem_speaker_level"] = MODEM_SPEAKER_LEVEL;
    doc["modem_audio_configured"] = modemAudioConfigured;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

/* =========================================================================
 * Panel Integration & Webhooks
 * ========================================================================= */

void registerDeviceToPanel() {
    if (WiFi.status() != WL_CONNECTED || currentICCID == "") return;
    lastRegistrationAttempt = millis();
    panelRegistered = false;
    registrationRetryMs = registrationRetryMs == 0 ? 30000 :
                          min(registrationRetryMs * 2, 300000UL);
    if (!audioRateVerified) {
        SerialMon.println("[PANEL] Registration waiting for the 10-second audio clock test.");
        return;
    }
    if (!strlen(DEVICE_REGISTRATION_TOKEN) || boardDeviceToken.length() != 64) {
        SerialMon.println("[PANEL] Registration unavailable: configure registration token and persistent board identity.");
        return;
    }
    
    HTTPClient http;
    String base = PANEL_URL;
    while (base.endsWith("/")) base.remove(base.length() - 1);
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    if (!http.begin(base + "/api/devices/register")) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEVICE_REGISTRATION_TOKEN);

    StaticJsonDocument<768> doc;
    doc["device_id"] = boardDeviceId;
    doc["device_token"] = boardDeviceToken;
    doc["call_events_version"] = 1;
    doc["iccid"] = currentICCID;
    doc["ip"] = WiFi.localIP().toString();
    doc["phone"] = currentPhone;
    JsonObject audio = doc.createNestedObject("audio");
    audio["format"] = "pcm_s16le";
    audio["channels"] = 1;
    audio["sample_rate"] = AUDIO_OUTPUT_SAMPLE_RATE;

    String requestBody;
    serializeJson(doc, requestBody);
    
    // Never log the body: it now contains the per-device credential.
    SerialMon.printf("[PANEL] Registering board %s (call events v1, audio %u Hz)\n",
                     boardDeviceId.c_str(), AUDIO_OUTPUT_SAMPLE_RATE);
    int httpCode = http.POST(requestBody);
    SerialMon.printf("[PANEL] Registration HTTP Code: %d\n", httpCode);
    if (httpCode < 0) {
        SerialMon.printf("[PANEL] Transport error: %s; verify panel host, port and LAN reachability.\n",
                         HTTPClient::errorToString(httpCode).c_str());
    } else if (httpCode == 401 || httpCode == 403) {
        SerialMon.println("[PANEL] Panel rejected registration credentials; check DEVICE_REGISTRATION_TOKEN.");
    }
    if (httpCode >= 200 && httpCode < 300) {
        panelRegistered = true;
        registeredIP = WiFi.localIP().toString();
        registrationRetryMs = 0;
        stateSyncRequested = true;
    }
    http.end();
}

void forwardSmsToWebhook(const String& sender, const String& message, const String& timestamp) {
    if (WiFi.status() != WL_CONNECTED) {
        SerialMon.println("[ERROR] WiFi not connected. Cannot forward SMS.");
        return;
    }

    HTTPClient http;
    http.begin(String(PANEL_URL) + "/webhook/sms");
    http.addHeader("Content-Type", "application/json; charset=utf-8");

    StaticJsonDocument<512> doc;
    doc["iccid"] = currentICCID;
    doc["sender"] = sender;
    doc["message"] = message;
    doc["timestamp"] = timestamp;

    String requestBody;
    serializeJson(doc, requestBody);

    SerialMon.println("\n[WEBHOOK] Forwarding SMS...");
    SerialMon.println(requestBody);
    
    int httpCode = http.POST(requestBody);
    
    if (httpCode >= 200 && httpCode < 300) {
        SerialMon.printf("[WEBHOOK] Success. HTTP Code: %d\n", httpCode);
    } else if (httpCode > 0) {
        SerialMon.printf("[WEBHOOK] Rejected. HTTP Code: %d; SMS delivery is not confirmed. Check panel logs.\n", httpCode);
    } else {
        SerialMon.printf("[WEBHOOK] Failed. Error: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

void handleListen() {
    server.send_P(200, "text/html; charset=utf-8", LISTEN_PAGE);
}

struct AudioLowPass {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void configure(float sampleRate, float cutoff, float q) {
        const float omega = 2.0f * 3.14159265358979323846f * cutoff / sampleRate;
        const float cosine = cosf(omega);
        const float alpha = sinf(omega) / (2.0f * q);
        const float inverseA0 = 1.0f / (1.0f + alpha);
        b0 = ((1.0f - cosine) * 0.5f) * inverseA0;
        b1 = (1.0f - cosine) * inverseA0;
        b2 = b0;
        a1 = (-2.0f * cosine) * inverseA0;
        a2 = (1.0f - alpha) * inverseA0;
        z1 = z2 = 0;
    }

    float process(float input) {
        const float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }
};

// Own the ADC and streaming socket here so slow AT/HTTP operations in loop()
// cannot starve audio. Continue draining DMA even when nobody is listening.
void audioStreamTask(void* parameter) {
    WiFiClient client;
    uint16_t raw[512];
    int16_t pcm[512];
    float dc = 2048.0f;
    bool dcInitialized = false;
    uint64_t calibrationSamples = 0;
    int64_t calibrationStartUs = 0;
    uint64_t resamplePhase = 0;
    AudioLowPass antiAliasStage1;
    AudioLowPass antiAliasStage2;
    bool calibrationPrimed = false;
    for (;;) {
        if (audioServer.hasClient()) {
            WiFiClient candidate = audioServer.available();
            candidate.setTimeout(1000);
            String request;
            unsigned long started = millis();
            while (candidate.connected() && millis() - started < 1000 && request.length() < 2048) {
                if (candidate.available()) {
                    request += (char)candidate.read();
                    if (request.endsWith("\r\n\r\n")) break;
                } else vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (!request.endsWith("\r\n\r\n") || !request.startsWith("GET / HTTP/")) {
                candidate.print("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                candidate.stop();
            } else if (client.connected()) {
                candidate.print("HTTP/1.1 409 Conflict\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                candidate.stop();
            } else {
                client.stop();
                client = candidate;
                client.setNoDelay(true);
                if (!audioRateVerified) {
                    client.print("HTTP/1.1 503 Service Unavailable\r\nRetry-After: 5\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    client.stop();
                } else {
                    String sessionId = randomId("stream");
                    uint64_t firstSample = getSampleCounter();
                    client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                  "X-Audio-Session-Id: %s\r\nX-Audio-Start-Sample: %llu\r\n"
                                  "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                                  sessionId.c_str(), (unsigned long long)firstSample);
                    SerialMon.printf("[AUDIO] Listener connected session=%s start=%llu\n",
                                     sessionId.c_str(), (unsigned long long)firstSample);
                }
            }
        }
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) {
            client.stop();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t rawCount = bytesRead / sizeof(uint16_t);
        if (!audioRateVerified) {
            // Discard the first returned DMA block. It may have accumulated
            // between driver installation and task startup and must not inflate
            // the ten-second hardware-rate measurement.
            if (!calibrationPrimed) {
                calibrationPrimed = true;
                calibrationStartUs = esp_timer_get_time();
                continue;
            }
            calibrationSamples += rawCount;
            int64_t elapsed = esp_timer_get_time() - calibrationStartUs;
            if (elapsed >= 10000000) {
                measuredRawSampleRate = (uint32_t)((calibrationSamples * 1000000ULL + elapsed / 2) / elapsed);
                audioRateVerified = measuredRawSampleRate >= AUDIO_OUTPUT_SAMPLE_RATE;
                SerialMon.printf("[AUDIO] 10s clock test: %llu samples in %.3fs = %u raw samples/s; output=%u Hz (%s).\n",
                                 (unsigned long long)calibrationSamples, elapsed / 1000000.0,
                                 measuredRawSampleRate, AUDIO_OUTPUT_SAMPLE_RATE,
                                 audioRateVerified ? "verified by filtered decimation" : "FAILED");
                if (!audioRateVerified) audioReady = false;
                if (audioRateVerified) {
                    const float cutoff = min(3400.0f, AUDIO_OUTPUT_SAMPLE_RATE * 0.42f);
                    // Two Butterworth sections form a fourth-order anti-alias
                    // filter before samples are discarded by the decimator.
                    antiAliasStage1.configure(measuredRawSampleRate, cutoff, 0.5411961f);
                    antiAliasStage2.configure(measuredRawSampleRate, cutoff, 1.3065630f);
                    resamplePhase = 0;
                }
                panelRegistered = false; // Re-register with verified stream metadata.
                registrationRetryMs = 0;
            }
            continue;
        }
        size_t pcmCount = 0;
        for (size_t i = 0; i < rawCount; ++i) {
            const float sample = raw[i] & 0x0fff;
            if (!dcInitialized) { dc = sample; dcInitialized = true; }
            dc += 0.005f * (sample - dc);
            const float centered = (sample - dc) * 16.0f;
            const float filtered = antiAliasStage2.process(antiAliasStage1.process(centered));
            resamplePhase += AUDIO_OUTPUT_SAMPLE_RATE;
            if (resamplePhase >= measuredRawSampleRate) {
                resamplePhase -= measuredRawSampleRate;
                pcm[pcmCount++] = (int16_t)constrain(filtered, -32768.0f, 32767.0f);
            }
        }
        portENTER_CRITICAL(&sampleCounterMux);
        sampleCounter += pcmCount;
        portEXIT_CRITICAL(&sampleCounterMux);
        if (client.connected()) {
            size_t sent = 0;
            size_t pcmBytes = pcmCount * sizeof(int16_t);
            unsigned long started = millis();
            while (sent < pcmBytes && client.connected() && millis() - started < 500) {
                size_t count = client.write(reinterpret_cast<const uint8_t*>(pcm) + sent, pcmBytes - sent);
                if (!count) break;
                sent += count;
            }
            // Never leave a partial sample in the connection after a failed write.
            if (sent != pcmBytes) client.stop();
        }
    }
}

void processIncomingUARTLine(const String& line) {
    // Typical SMS format: +CMT: "+989123456789","","23/10/25,14:35:00+14"
    // Followed by SMS text on the next line.
    static String sender = "";
    static String timestamp = "";
    static bool readingText = false;

    if (readingText) {
        String text = line;
        readingText = false;
        
        // Some firmwares output Persian SMS as pure UCS2 Hex string. Let's decode it if detected.
        if (isPureHexString(text)) {
            text = ucs2HexToUtf8(text);
        }
        
        SerialMon.printf("\n[SMS RCV] From: %s | Text: %s\n", sender.c_str(), text.c_str());
        forwardSmsToWebhook(sender, text, timestamp);
        return;
    }

    if (line.startsWith("+CMT:")) {
        int firstQuote = line.indexOf('"');
        int secondQuote = line.indexOf('"', firstQuote + 1);
        if (firstQuote != -1 && secondQuote != -1) {
            sender = line.substring(firstQuote + 1, secondQuote);
        }
        
        int thirdQuote = line.indexOf('"', secondQuote + 1); // skip empty string field
        int fourthQuote = line.indexOf('"', thirdQuote + 1);
        int fifthQuote = line.indexOf('"', fourthQuote + 1); // start of timestamp
        int sixthQuote = line.indexOf('"', fifthQuote + 1);
        
        if (fifthQuote != -1 && sixthQuote != -1) {
            timestamp = line.substring(fifthQuote + 1, sixthQuote);
        }
        
        readingText = true; // Mark that next line from UART will be the SMS content
        return;
    }

    // --- Voice Call Handling ---
    if (line.startsWith("+CLIP:")) {
        int first = line.indexOf('"');
        int second = line.indexOf('"', first + 1);
        if (first >= 0 && second > first) callerNumber = line.substring(first + 1, second);
        return;
    }

    if (line.startsWith("+CLCC:")) {
        int firstComma = line.indexOf(',');
        int secondComma = line.indexOf(',', firstComma + 1);
        int thirdComma = line.indexOf(',', secondComma + 1);
        if (secondComma > 0 && thirdComma > secondComma) {
            int stat = line.substring(secondComma + 1, thirdComma).toInt();
            if (stat == 0) {
                callSeenInLastClcc = true;
                if (!currentCallId.length()) currentCallId = randomId("call");
                if (callState != CALL_ACTIVE) {
                    callState = CALL_ACTIVE;
                    isRecording = true;
                    queueCallEvent("answered");
                    SerialMon.println("[CALL] Modem confirmed active call (CLCC stat=0).");
                }
            } else if (stat == 4 || stat == 5) {
                callSeenInLastClcc = true;
            }
        }
        return;
    }

    if (line == "RING") {
        if (callState == CALL_IDLE) {
            currentCallId = randomId("call");
            callerNumber = "";
            callState = CALL_RINGING;
            queueCallEvent("ringing");
            SerialMon.printf("\n[CALL] Incoming call %s; ringing event queued.\n", currentCallId.c_str());
            SerialAT.println("ATA");
            callState = CALL_ANSWERING;
            answeringStarted = millis();
            SerialMon.println("[CALL] ATA sent; waiting for CLCC stat=0 before answered event.");
        }
        return;
    }

    if (line == "NO CARRIER") {
        isRecording = false;
        finishCall(callState == CALL_ACTIVE ? "remote_hangup" : "no_answer");
        return;
    }
    if (line == "BUSY") {
        isRecording = false;
        finishCall("busy");
        return;
    }
    if (line == "NO ANSWER") {
        isRecording = false;
        finishCall("no_answer");
        return;
    }
}

/* =========================================================================
 * Setup & Loop
 * ========================================================================= */

void setup() {
    SerialMon.begin(115200);
    delay(1000);
    SerialMon.println("\n=======================================================");
    SerialMon.println(" TTGO T-Call SMS Gateway (WebServer & Webhook)         ");
    SerialMon.println("=======================================================");

    bootId = randomId("boot");
    callEventQueue = xQueueCreate(16, sizeof(CallEventMessage));
    if (!callEventQueue || xTaskCreate(callEventSenderTask, "call-events", 7168, nullptr, 1, nullptr) != pdPASS)
        SerialMon.println("[EVENT] Failed to initialize call event sender.");

    initModem();
    initI2S();

    // SerialMon.printf("[WIFI] Configuring Static IP...\n");
    // if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    //     SerialMon.println("[WIFI WARN] Failed to configure Static IP");
    // }

    SerialMon.printf("[WIFI] Connecting to '%s'...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        SerialMon.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        SerialMon.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        SerialMon.println("\n[WIFI WARN] Connection Failed. Check credentials.");
    }
    if (!initPanelIdentity()) SerialMon.println("[PANEL] Failed to initialize persistent device credential.");

    // Web Server Routes
    server.on("/send-sms", HTTP_POST, handleSendSMS);
    server.on("/contacts", HTTP_GET, handleGetContacts);
    server.on("/health", HTTP_GET, handleHealthCheck);
    server.on("/listen", HTTP_GET, handleListen);
    server.begin();
    SerialMon.println("[WEB] Web Server Started on Port 80");

    // Start Audio Stream Server
    audioServer.begin();
    if (audioReady && xTaskCreate(audioStreamTask, "audio-stream", 6144, nullptr, 1, nullptr) != pdPASS) {
        audioReady = false;
        SerialMon.println("[AUDIO] Failed to start streaming task");
    }
    SerialMon.println("[STREAM] Audio Stream Server Started on Port 8080");

    currentState = SIM_MISSING;
}

void loop() {
    // Keep WiFi alive
    if (WiFi.status() != WL_CONNECTED) {
        panelRegistered = false;
        // PlatformIO default ESP32 core will attempt to auto-reconnect,
        // but can be explicitly handled here if needed.
    }
    
    // Process WebServer Clients
    server.handleClient();

    // Check for incoming UART lines (for SMS or AT unsolicited messages)
    while (SerialAT.available()) {
        char c = SerialAT.read();
        if (c == '\n') {
            incomingSmsBuffer.trim();
            if (incomingSmsBuffer.length() > 0) {
                processIncomingUARTLine(incomingSmsBuffer);
            }
            incomingSmsBuffer = "";
        } else if (c != '\r') {
            incomingSmsBuffer += c;
        }
    }

    // State Machine for SIM hot-swapping and readiness
    switch (currentState) {
        case SIM_MISSING:
            if (millis() - lastSimCheckTime > 3000) {
                lastSimCheckTime = millis();
                sendAT("AT", 500, true); // Wake up modem & sync baud rate
                String cpin = sendAT("AT+CPIN?", 2000, true);
                if (cpin.indexOf("READY") != -1) {
                    SerialMon.println("[SIM] SIM Card Detected. Initializing...");
                    currentState = SIM_INIT;
                    simCheckFailCount = 0;
                }
            }
            break;

        case SIM_INIT: {
            sendAT("AT", 1000, true);
            sendAT("ATE0", 1000, true);          // Disable Echo
            sendAT("AT+CMGF=1", 1000, true);     // Text mode
            sendAT("AT+CSCS=\"GSM\"", 1000, true); // GSM char set
            sendAT("AT+CNMI=2,2,0,0,0", 1000, true); // Forward SMS to serial directly
            sendAT("AT+CLIP=1", 1000, true);      // Caller ID URC

            // Audio Configuration for Calls
            String channelResponse = sendAT(String("AT+CHFA=") + MODEM_AUDIO_CHANNEL, 1000, true);
            // Gain at the modem improves ADC SNR; backend normalization cannot
            // recover speech detail already buried below the analog noise floor.
            String volumeResponse = sendAT(String("AT+CLVL=") + MODEM_SPEAKER_LEVEL, 1000, true);
            modemAudioConfigured = channelResponse.endsWith("OK") && volumeResponse.endsWith("OK");
            SerialMon.printf("[AUDIO] Analog channel %d, speaker level %d: %s\n",
                             MODEM_AUDIO_CHANNEL, MODEM_SPEAKER_LEVEL,
                             modemAudioConfigured ? "OK (external wiring still required)" : "FAILED");
            
            SerialMon.println("[SIM] Modem configured for SMS and Voice Calls. Waiting for network...");
            
            // Allow some time to register to network
            delay(3000); 
            
            currentICCID = getICCID();
            currentPhone = "";
            String cnum = sendAT("AT+CNUM", 2000, true);
            int firstQuote = cnum.indexOf(",\"");
            if (firstQuote != -1) {
                int endQuote = cnum.indexOf('"', firstQuote + 2);
                if (endQuote != -1) currentPhone = cnum.substring(firstQuote + 2, endQuote);
            }
            registrationRetryMs = 0;
            SerialMon.printf("[SIM] SIM is READY. ICCID: %s\n", currentICCID.c_str());
            registerDeviceToPanel();

            currentState = SIM_READY;
            SerialMon.println("[SIM] Listening for messages...");
            break;
        }

        case SIM_READY:
            // Check SIM status every 10 seconds to detect removal
            if (millis() - lastSimCheckTime > 10000) {
                lastSimCheckTime = millis();
                String cpin = sendAT("AT+CPIN?", 2000, true);
                if (cpin.indexOf("READY") != -1) {
                    simCheckFailCount = 0;
                } else if (cpin.indexOf("ERROR") != -1 || cpin.length() == 0) {
                    simCheckFailCount++;
                    SerialMon.printf("[SIM] Warning: CPIN check failed (%d/3)\n", simCheckFailCount);
                    if (simCheckFailCount >= 3) {
                        SerialMon.println("[SIM] SIM Card removed or error! Going to MISSING state.");
                        currentState = SIM_MISSING;
                        modemAudioConfigured = false;
                        panelRegistered = false;
                        currentICCID = "";
                        currentPhone = "";
                        simCheckFailCount = 0;
                    }
                }
            }
            break;
    }
    if (currentState == SIM_READY && WiFi.status() == WL_CONNECTED) {
        if (panelRegistered && registeredIP != WiFi.localIP().toString()) {
            panelRegistered = false;
            registrationRetryMs = 0;
        }
        if (!panelRegistered && millis() - lastRegistrationAttempt >= registrationRetryMs)
            registerDeviceToPanel();
    }

    // Modem state, never audio energy, controls the call lifecycle.
    if (currentState == SIM_READY && callState != CALL_IDLE && millis() - lastClccPoll >= 700) {
        lastClccPoll = millis();
        callSeenInLastClcc = false;
        String clcc = sendAT("AT+CLCC", 900, true);
        if (callState == CALL_ACTIVE) {
            if (callSeenInLastClcc) missingActiveClccPolls = 0;
            else if (++missingActiveClccPolls >= 2) {
                isRecording = false;
                finishCall("remote_hangup");
            }
        } else if (callState == CALL_ANSWERING && millis() - answeringStarted > 45000) {
            finishCall("no_answer");
        }
    }
    if (stateSyncRequested && currentState == SIM_READY) {
        callSeenInLastClcc = false;
        sendAT("AT+CLCC", 900, true);
        if (callSeenInLastClcc && !currentCallId.length()) currentCallId = randomId("call");
        sendCurrentStateEvent();
        stateSyncRequested = false;
    }
}
