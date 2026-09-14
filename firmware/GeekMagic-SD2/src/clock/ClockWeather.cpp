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

static ServiceStatus s_services[ClockWeather::MAX_SERVICES];
static int s_serviceCount = 0;

static Weather s_weather;
static unsigned long s_weatherNext = 0;   // 下次允许拉取的 earliest 时间 (0=立即)
static unsigned long s_serviceNext = 0;   // 下次开始一轮服务探测的时间
static int s_serviceIdx = -1;             // 正在探测的服务下标, -1=当前无进行中的探测轮

// ESP8266 WiFiClient::connect 在宿主机 DOWN 时会阻塞整个下载 timeout
// (ClientContext::connect 内 esp_delay), 主循环(含每秒时钟渲染)会一直被卡住,
// 且多个 DOWN 服务连加可能触发 2s 看门狗. 因此每个探测单独设一个较短的上限,
// 且一次 update() 至多探测一个服务, 把阻塞分摊到多次主循环迭代里.
static constexpr int SERVICE_PROBE_TIMEOUT_MS = 500;

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
    return WiFi.status() == WL_CONNECTED;
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

    String body;
    Stream& stream = http.getStream();
    while (stream.available()) {
        int b = stream.read();
        if (b < 0) break;
        body += (char)b;
        if (body.length() > 800) break;
    }
    http.end();

    // 响应可能带 chunked 传输的 chunk-size 行等前缀, 从第一个 '{' 起解析 JSON
    int start = body.indexOf('{');
    if (start < 0) {
        Logger::warn("weather: no JSON object in body", TAG);
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.substring(start));
    if (err) {
        Logger::warn(("weather parse failed: " + String(err.c_str())).c_str(), TAG);
        return;
    }

    s_weather.ok = true;
    s_weather.temp = doc["current"]["temperature_2m"] | 0.0f;
    s_weather.code = doc["current"]["weather_code"] | -1;
    s_weather.desc = codeToText(s_weather.code);
    Logger::info(("weather " + s_weather.desc + " " + String(s_weather.temp, 1) + "C").c_str(), TAG);
}

// 探测 s_services[s_serviceIdx] 一个服务并推进到下一个, 返回该轮是否全部完成
static bool probeOneService() {
    WiFiClient c;
    c.setTimeout(SERVICE_PROBE_TIMEOUT_MS);
    unsigned long t0 = millis();
    int r = c.connect(s_services[s_serviceIdx].ip.c_str(), s_services[s_serviceIdx].port);
    unsigned long elapsed = millis() - t0;
    if (r == 1) {
        s_services[s_serviceIdx].up = true;
        s_services[s_serviceIdx].last_ms = (unsigned long)t0;
        s_services[s_serviceIdx].latency_ms = (int)elapsed;
    } else {
        s_services[s_serviceIdx].up = false;
        s_services[s_serviceIdx].last_ms = 0;
        s_services[s_serviceIdx].latency_ms = -1;
    }
    c.stop();
    Logger::info(("svc " + s_services[s_serviceIdx].ip + ":" + String(s_services[s_serviceIdx].port) +
                  (s_services[s_serviceIdx].up ? (" UP " + String(elapsed) + "ms") : " DOWN")).c_str(), TAG);
    s_serviceIdx++;
    yield();
    return s_serviceIdx >= s_serviceCount;
}

void begin() {
    parseServices();
    s_weatherNext = 0;   // 首次立即拉取
    s_serviceNext = 0;
    s_serviceIdx = -1;
}

void update() {
    if (!wifiUp()) return;

    unsigned long now = millis();
    unsigned long wInt = (unsigned long)configManager.weather_min * 60000UL;
    unsigned long sInt = (unsigned long)configManager.service_sec * 1000UL;

    // 服务探测: 每轮按 service_sec 调度, 但一次 update() 至多探测一个服务.
    // 逐服务分摊阻塞时间, 避免 DOWN 服务把主循环(时钟渲染)卡死或触发看门狗.
    if (s_serviceCount > 0) {
        if (s_serviceIdx < 0 && now >= s_serviceNext) {
            s_serviceIdx = 0;   // 开始新一轮
        }
        if (s_serviceIdx >= 0 && probeOneService()) {
            s_serviceIdx = -1;
            s_serviceNext = millis() + sInt;
        }
    } else {
        s_serviceIdx = -1;
    }
    if (now >= s_weatherNext) {
        fetchWeather();
        s_weatherNext = millis() + wInt;
    }
}

void forceWeather() {
    if (wifiUp()) fetchWeather();
}

unsigned long nextWeatherMs() {
    return s_weatherNext;
}

}  // namespace ClockWeather
