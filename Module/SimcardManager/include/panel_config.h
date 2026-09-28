#pragma once

// Local secrets/configuration are deliberately excluded from version control.
#if __has_include("panel_config.local.h")
#include "panel_config.local.h"
#endif

// --- Network & Config ---
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID "Otaq"
#endif

#ifndef CONFIG_WIFI_PASS
#define CONFIG_WIFI_PASS "Pour1412#"
#endif

// --- Static IP Configuration ---
#ifndef CONFIG_STATIC_IP
#define CONFIG_STATIC_IP 10, 10, 30, 201
#endif
#ifndef CONFIG_STATIC_GATEWAY
#define CONFIG_STATIC_GATEWAY 10, 10, 30, 1
#endif
#ifndef CONFIG_STATIC_SUBNET
#define CONFIG_STATIC_SUBNET 255, 255, 255, 0
#endif
#ifndef CONFIG_STATIC_DNS1
#define CONFIG_STATIC_DNS1 8, 8, 8, 8
#endif
#ifndef CONFIG_STATIC_DNS2
#define CONFIG_STATIC_DNS2 8, 8, 4, 4
#endif

#ifndef PANEL_BASE_URL
#define PANEL_BASE_URL "http://10.10.30.50:3001"
#endif
#ifndef DEVICE_REGISTRATION_TOKEN
#define DEVICE_REGISTRATION_TOKEN "change-me"
#endif
#ifndef AUDIO_OUTPUT_SAMPLE_RATE
#define AUDIO_OUTPUT_SAMPLE_RATE 44240
#endif
// static_assert(AUDIO_OUTPUT_SAMPLE_RATE == 8000 || AUDIO_OUTPUT_SAMPLE_RATE == 48000,
//               "Audio output must use a supported telephony rate");
#ifndef MODEM_SPEAKER_LEVEL
// Raise the modem signal before the ESP32 ADC. This improves signal-to-noise
// ratio more effectively than applying large digital gain to a quiet capture.
#define MODEM_SPEAKER_LEVEL 100
#endif
static_assert(MODEM_SPEAKER_LEVEL >= 0 && MODEM_SPEAKER_LEVEL <= 100,
              "SIM800 speaker level must be between 0 and 100");
