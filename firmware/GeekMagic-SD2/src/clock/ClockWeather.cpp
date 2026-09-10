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

#include "clock/ClockWeather.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <Logger.h>
#include <wireless/WiFiManager.h>
#include "config/ConfigManager.h"

extern ConfigManager configManager;

static constexpr const char* TAG = "ClockWeather";

namespace ClockWeather {

static const int MAX_SERVICES = 4;
static ServiceStatus s_services[MAX_SERVICES];
static int s_serviceCount = 0;

static Weather s_weather;
static unsigned long s_weatherTs = 0;
static unsigned long s_serviceTs = 0;

static const char* WMO_TEXT[] = {
    "晴", "多云", "阴", "小雨", "中雨", "大雨", "暴雨", "阵雨",
    "阵雨", "阵雨", "雪", "小雪", "大雪", "暴雪", "冰粒", "冻雨",
    "冻雨", "冻雨", "雷阵雨", "雷阵雨", "雷阵雨", "雷雨", "雷雨",
    "雾", "雾", "薄雾", "烟雾", "沙尘", "沙尘暴", "沙尘暴",
    "零星小雨", "零星小雨", "零星雪", "零星雪", "零星阵雨",
    "零星阵雨", "冰粒", "冰粒", "强对流", "强对流",
};

const char* codeToText(int code) {
    if (code < 0) return "--";
    if (code >= 0 && code <= 1) return "晴";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code >= 45 && code <= 48) return "雾";
    if (code >= 51 && code <= 57) return "小雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 85 && code <= 86) return "雪";
    if (code >= 95 && code <= 99) return "雷雨";
    if (code >= 0 && code < (int)(sizeof(WMO_TEXT) / sizeof(WMO_TEXT[0]))) return WMO_TEXT[code];
    return "未知";
}

const char* weatherDesc() {
    return s_weather.desc.c_str();
}

const Weather& weather() {
    return s_weather;
}

const ServiceStatus* services(int& count) {
    count = s_serviceCount;
    return s_services;
}

Clock localClock() {
    Clock c{};
    time_t now = time(nullptr);
    if (now < 1600000000L) return c;  // not synced

    time_t local = now + configManager.tz_offset * 3600L;
    struct tm t;
    // gmtime_r 恒按 UTC 解析; 我们已把 epoch 偏移到目标时区
    gmtime_r(&local, &t);

    c.valid = true;
    c.hour = t.tm_hour;
    c.min = t.tm_min;
    c.sec = t.tm_sec;
    c.year = t.tm_year + 1900;
    c.mon = t.tm_mon + 1;
    c.day = t.tm_mday;
    c.weekday = t.tm_wday;
    return c;
}

static bool wifiUp() {
    extern WiFiManager* wifiManager;
    return wifiManager != nullptr && WiFiManager::isConnected() && !wifiManager->isApMode();
}

static void parseServices() {
    s_serviceCount = 0;
    String svcs = String(configManager.getServices());
    int start = 0;
    for (int i = 0; i <= svcs.length() && s_serviceCount < MAX_SERVICES; i++) {
        if (i == svcs.length() || svcs[i] == ',') {
            if (i > start) {
                String tok = svcs.substring(start, i);
                int colon = tok.indexOf(':');
                ServiceStatus s{};
                if (colon >= 0) {
                    s.ip = tok.substring(0, colon);
                    s.port = tok.substring(colon + 1).toInt();
                } else {
                    s.ip = tok;
                    s.port = 80;
                }
                if (s.ip.length() > 0 && s.port > 0) {
                    s_services[s_serviceCount++] = s;
                }
            }
            start = i + 1;
        }
    }
}

static void fetchWeather() {
    char url[200];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&timezone=Asia%%2FShanghai",
             configManager.lat, configManager.lng);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(client, url)) {
        Logger::warn("weather begin failed", TAG);
        return;
    }
    const int status = http.GET();
    if (status != 200) {
        Logger::warn(("weather GET " + String(status)).c_str(), TAG);
        http.end();
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Logger::warn("weather parse failed", TAG);
        return;
    }

    s_weather.ok = true;
    s_weather.temp = doc["current"]["temperature_2m"] | 0.0f;
    s_weather.code = doc["current"]["weather_code"] | -1;
    s_weather.desc = codeToText(s_weather.code);
    s_weatherTs = millis();
    Logger::info(("weather " + s_weather.desc + " " + String(s_weather.temp, 1) + "C").c_str(), TAG);
}

static void checkServices() {
    for (int i = 0; i < s_serviceCount; i++) {
        WiFiClient c;
        c.setTimeout(2000);
        int r = c.connect(s_services[i].ip.c_str(), s_services[i].port);
        if (r == 1) {
            s_services[i].up = true;
            s_services[i].last_ms = millis();
            c.stop();
        } else {
            s_services[i].up = false;
            c.stop();
        }
        delay(50);
    }
}

void begin() {
    parseServices();
    s_weatherTs = 0;
    s_serviceTs = 0;
}

void update() {
    if (!wifiUp()) return;

    unsigned long now = millis();
    unsigned long wInt = (unsigned long)configManager.weather_min * 60000UL;
    unsigned long sInt = (unsigned long)configManager.service_sec * 1000UL;

    if (now - s_serviceTs >= sInt) {
        checkServices();
        s_serviceTs = now;
    }
    if (now - s_weatherTs >= wInt) {
        fetchWeather();
    }
}

void forceWeather() {
    if (wifiUp()) fetchWeather();
}

}  // namespace ClockWeather
