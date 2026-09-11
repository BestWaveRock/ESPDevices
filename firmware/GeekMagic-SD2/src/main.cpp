// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <ESP8266HTTPUpdateServer.h>

#include <Logger.h>
#include "project_version.h"
#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "display/DisplayManager.h"
#include "web/Webserver.h"
#include "web/Api.h"
#include "ntp/NTPClient.h"
#include "boot/RescueMode.h"
#include "dashboard/DashboardManager.h"
#include "clock/ClockWeather.h"
#include <array>

#ifndef METRICS_URL
#define METRICS_URL ""
#endif

ConfigManager configManager;
const char* AP_SSID = "GeekMagic";
const char* AP_PASSWORD = "$str0ngPa$$w0rd";
// Built-in fallback WiFi. Leave empty (default) to skip; the device then
// falls back to the "GeekMagic" AP for configuration. Fill in your own values
// at build time if you want first-boot auto-connect.
static constexpr const char* DEFAULT_WIFI_SSID = "";
static constexpr const char* DEFAULT_WIFI_PASSWORD = "";
WiFiManager* wifiManager = nullptr;
ESP8266HTTPUpdateServer httpUpdater;
static constexpr const char* KV_SALT_STR = "GeekMagicOpenFirmwareIsAwesome";
static size_t initial_free_heap = 0;
static constexpr size_t FREE_BUF_SIZE = 32;
static constexpr size_t MSG_BUF_SIZE = 96;

static constexpr uint32_t SERIAL_BAUD_RATE = 115200;
static constexpr uint32_t BOOT_DELAY_MS = 200;
static constexpr int LOADING_BAR_TEXT_X = 50;
static constexpr int LOADING_BAR_TEXT_Y = 80;
static constexpr int LOADING_BAR_Y = 110;
static constexpr int LOADING_DELAY_MS = 1000;
static constexpr const char* METRICS_ENDPOINT = METRICS_URL;

Webserver* webserver = nullptr;
NTPClient* ntpClient = nullptr;

/**
 * @brief Formats bytes into a human-readable string
 *
 * @param value Size in bytes
 * @return Formatted string
 */
static void formatBytes(size_t value, char* outBuf, size_t outBufSize) {
    constexpr std::array<const char*, 5> UNITS = {"B", "KB", "MB", "GB", "TB"};
    constexpr double THRESHOLD = 1024.0;

    auto val = static_cast<double>(value);
    int unit = 0;
    while (val >= THRESHOLD && unit < static_cast<int>(UNITS.size()) - 1) {
        val /= THRESHOLD;
        ++unit;
    }

    if (unit == 0) {
        snprintf(outBuf, outBufSize, "%u %s", static_cast<unsigned int>(value), UNITS[unit]);
    } else {
        snprintf(outBuf, outBufSize, "%.1f %s", val, UNITS[unit]);
    }
}

/**
 * @brief Check whether LittleFS contains at least one entry
 *
 * @return true if filesystem root has any file/dir entry
 */
static auto littleFsHasEntries() -> bool {
    Dir dir = LittleFS.openDir("/");
    return dir.next();
}

/**
 * @brief Initializes the system
 *
 */
void setup() {
    Serial.begin(SERIAL_BAUD_RATE);
    delay(BOOT_DELAY_MS);
    Serial.println("");
    Logger::info(("GeekMagic Open Firmware " + String(PROJECT_VER_STR)).c_str());

    constexpr int TOTAL_STEPS = 5;
    int step = 0;
    const bool littleFsMounted = LittleFS.begin();
    bool littleFsReadyForStatic = littleFsMounted;

    if (!littleFsMounted) {
        Logger::error("Failed to mount LittleFS");
        Logger::warn("LittleFS unavailable, static web UI disabled", "Global");
    } else if (!littleFsHasEntries()) {
        littleFsReadyForStatic = false;
        Logger::warn("LittleFS mounted but empty, static web UI disabled", "Global");
    }

    SecureStorage::setSalt(KV_SALT_STR);

    if (configManager.secure.begin()) {
        Logger::info("SecureStorage initialized successfully", "ConfigManager");
    }

    if (configManager.load()) {
        Logger::info("Configuration loaded successfully");
    }

    if (RescueMode::checkBootLoop()) {
        RescueMode::run();
        EspClass::wdtEnable(WDTO_2S);

        return;
    }

    step += 2;

    DisplayManager::begin();

    DisplayManager::drawLoadingBar((float)step / TOTAL_STEPS, LOADING_BAR_Y);

    step++;

    DisplayManager::drawTextWrapped(LOADING_BAR_TEXT_X, LOADING_BAR_TEXT_Y, "Starting...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar((float)step / TOTAL_STEPS, LOADING_BAR_Y);
    step++;

    wifiManager = new WiFiManager(configManager.getSSID(), configManager.getPassword(), AP_SSID, AP_PASSWORD);
    wifiManager->begin();

    if (!WiFiManager::isConnected() && DEFAULT_WIFI_SSID[0] != '\0') {
        Logger::info("No saved WiFi connected, trying built-in default WiFi", "Global");
        if (wifiManager->connectToNetwork(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD, 10000)) {
            configManager.setWiFi(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
            configManager.save();
            Logger::info("Connected to built-in default WiFi and saved", "Global");
        }
    }

    ntpClient = new NTPClient();
    ntpClient->begin();

    ClockWeather::begin();

    DisplayManager::drawLoadingBar((float)step / TOTAL_STEPS, LOADING_BAR_Y);

    step++;

    webserver = new Webserver();
    webserver->begin();

    initial_free_heap = ESP.getFreeHeap();  // NOLINT(readability-static-accessed-through-instance)

    DisplayManager::drawLoadingBar((float)step / TOTAL_STEPS, LOADING_BAR_Y);

    registerApiEndpoints(webserver);

    // /config 页面: 读 config.json 展示配置表单, POST 写回 LittleFS 并重启
    if (littleFsReadyForStatic && webserver != nullptr) {
        webserver->raw().on("/config", HTTP_GET, []() {
            String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Config</title>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>body{font-family:sans-serif;max-width:600px;margin:2rem auto;padding:0 1rem}"
                "label{display:block;margin:0.7rem 0 0.2rem}input,select{width:100%;padding:0.4rem}"
                "button{background:#2563eb;color:#fff;border:none;padding:0.6rem 1.2rem;border-radius:4px;margin-top:1rem}"
                "h1{color:#2563eb}</style></head><body>"
                "<h1>Clock / Weather / Service Config</h1>"
                "<form method='POST' action='/config'>"
                "<label>Latitude (lat)</label><input name='lat' type='number' step='0.0001' value='" + String(configManager.lat) + "'>"
                "<label>Longitude (lng)</label><input name='lng' type='number' step='0.0001' value='" + String(configManager.lng) + "'>"
                "<label>City</label><input name='city' type='text' value='" + String(configManager.getCity()) + "'>"
                "<label>Services (comma, ip:port)</label><input name='services' type='text' value='" + String(configManager.getServices()) + "'>"
                "<label>Weather interval (minutes)</label><input name='weather_min' type='number' value='" + String(configManager.weather_min) + "'>"
                "<label>Service check interval (seconds)</label><input name='service_sec' type='number' value='" + String(configManager.service_sec) + "'>"
                "<label>Timezone offset (hours)</label><input name='tz_offset' type='number' value='" + String(configManager.tz_offset) + "'>"
                "<label>IP font size (px, 8-24)</label><input name='ipFontSize' type='number' min='8' max='24' value='" + String(configManager.ipFontSize) + "'>"
                "<label>Date font size (px, 8-24)</label><input name='dateFontSize' type='number' min='8' max='24' value='" + String(configManager.dateFontSize) + "'>"
                "<label>Clock font size (px, 17-68)</label><input name='clockFontSize' type='number' min='17' max='68' value='" + String(configManager.clockFontSize) + "'>"
                "<label>Lunar font size (px, 16-48)</label><input name='lunarFontSize' type='number' min='16' max='48' value='" + String(configManager.lunarFontSize) + "'>"
                "<label>Weather font size (px, 16-48)</label><input name='weatherFontSize' type='number' min='16' max='48' value='" + String(configManager.weatherFontSize) + "'>"
                "<label>Service font size (px, 8-24)</label><input name='serviceFontSize' type='number' min='8' max='24' value='" + String(configManager.serviceFontSize) + "'>"
                "<label><input name='reverseMap' type='checkbox'" + String(configManager.reverseMap ? " checked" : "") + "> GLCD reverse map (fix black lines on non-integer scaling)</label>"
                "<button type='submit'>Save &amp; Reboot</button>"
                "</form></body></html>";
            webserver->raw().send(200, "text/html; charset=UTF-8", html.c_str());
        });
        webserver->raw().on("/config", HTTP_POST, []() {
            auto& server = webserver->raw();
            if (!server.hasArg("city")) {
                server.send(400, "text/plain", "Missing city");
                return;
            }
            configManager.lat = server.arg("lat").toFloat();
            configManager.lng = server.arg("lng").toFloat();
            configManager.city = server.arg("city").c_str();
            configManager.services = server.arg("services").c_str();
            configManager.weather_min = server.arg("weather_min").toInt();
            configManager.service_sec = server.arg("service_sec").toInt();
            configManager.tz_offset = server.arg("tz_offset").toInt();
            configManager.ipFontSize = server.arg("ipFontSize").toInt();
            if (configManager.ipFontSize < 8 || configManager.ipFontSize > 24) configManager.ipFontSize = 8;
            configManager.dateFontSize = server.arg("dateFontSize").toInt();
            if (configManager.dateFontSize < 8 || configManager.dateFontSize > 24) configManager.dateFontSize = 8;
            configManager.clockFontSize = server.arg("clockFontSize").toInt();
            if (configManager.clockFontSize < 17 || configManager.clockFontSize > 68) configManager.clockFontSize = 34;
            configManager.lunarFontSize = server.arg("lunarFontSize").toInt();
            if (configManager.lunarFontSize < 16 || configManager.lunarFontSize > 48) configManager.lunarFontSize = 16;
            configManager.weatherFontSize = server.arg("weatherFontSize").toInt();
            if (configManager.weatherFontSize < 16 || configManager.weatherFontSize > 48) configManager.weatherFontSize = 18;
            configManager.serviceFontSize = server.arg("serviceFontSize").toInt();
            if (configManager.serviceFontSize < 8 || configManager.serviceFontSize > 24) configManager.serviceFontSize = 8;
            configManager.reverseMap = server.hasArg("reverseMap") && server.arg("reverseMap") == "on";
            configManager.save();
            server.send(200, "text/html; charset=UTF-8",
                        "<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='3;url=/config'>"
                        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        "<style>body{font-family:sans-serif;background:#111;color:#fff;display:flex;align-items:center;"
                        "justify-content:center;height:100vh;margin:0;flex-direction:column}"
                        "h1{font-size:1.2rem;margin:0 0 1rem}.cd{font-size:3rem;margin:0 1rem;color:#4ade80}</style>"
                        "</head><body><h1>配置已保存，设备重启中...</h1>"
                        "<p><span class='cd' id='cd'>3</span>秒后自动刷新</p>"
                        "<script>var n=3,el=document.getElementById('cd');setInterval(function(){n--;"
                        "if(n>0){el.textContent=n}else{location.href='/config'}},1000);</script>"
                        "</body></html>");
            delay(500);
            ESP.restart();
        });
        Logger::info("Registered /config page", "Global");
    }

    if (!littleFsReadyForStatic) {
        httpUpdater.setup(&webserver->raw(), "/legacyupdate");
        Logger::warn("Enabled legacy OTA route because LittleFS is unavailable or empty", "Global");
    } else {
        webserver->serveStaticC("/", "/web/index.html", "text/html");
        webserver->serveStaticC("/config.json", "/config.json", "application/json");
        webserver->registerGenericStaticFallback("/web", true);
    }

    DisplayManager::drawLoadingBar(1.0F, LOADING_BAR_Y);

    delay(LOADING_DELAY_MS);

    DisplayManager::drawStartup(wifiManager->getIP().toString());

    if (METRICS_ENDPOINT[0] != '\0' && WiFiManager::isConnected() && !wifiManager->isApMode()) {
        DashboardManager::begin(METRICS_ENDPOINT);
    }

    // enable watchdog before going to loop()
    // 2 seconds should be way more than the main loop needs to do stuff
    EspClass::wdtEnable(WDTO_2S);
}

void loop() {
    if (RescueMode::isActive()) {
        RescueMode::loop();
        return;
    }

    static bool bootStableMarked = false;
    if (!bootStableMarked && millis() >= BOOT_STABLE_MS) {
        RescueMode::markBootStable();
        bootStableMarked = true;
    }

    if (webserver != nullptr) {
        webserver->handleClient();
    }

    if (ntpClient != nullptr) {
        ntpClient->loop();
    }

    ClockWeather::update();

    DisplayManager::update();

    if (METRICS_ENDPOINT[0] != '\0' && wifiManager != nullptr && WiFiManager::isConnected() &&
        !wifiManager->isApMode()) {
        DashboardManager::update();
    }

    static unsigned long last_free_heap_log = 0;
    static constexpr unsigned long FREE_HEAP_LOG_INTERVAL_MS = 10000UL;
    unsigned long now = millis();

    if (now - last_free_heap_log >= FREE_HEAP_LOG_INTERVAL_MS) {
        last_free_heap_log = now;
        char freeBuf[FREE_BUF_SIZE];
        char initBuf[FREE_BUF_SIZE];
        char msgBuf[MSG_BUF_SIZE];

        formatBytes(ESP.getFreeHeap(), freeBuf,  // NOLINT(readability-static-accessed-through-instance)
                    sizeof(freeBuf));
        formatBytes(initial_free_heap, initBuf, sizeof(initBuf));

        snprintf(msgBuf, sizeof(msgBuf), "Free heap: %s (initial: %s)", freeBuf, initBuf);
        Logger::info(msgBuf);
    }

    EspClass::wdtFeed();  // kick watchdog
}
