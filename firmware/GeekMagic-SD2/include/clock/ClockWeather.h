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

#ifndef CLOCK_WEATHER_H
#define CLOCK_WEATHER_H

#include <Arduino.h>
#include <cstdint>
#include <ctime>

namespace ClockWeather {
    struct ServiceStatus {
        String ip;
        int port = 80;
        bool up = false;
        unsigned long last_ms = 0;  // 0 = unknown
        int latency_ms = -1;        // -1 = unknown
    };

    constexpr int MAX_SERVICES = 8;

    struct Weather {
        bool ok = false;
        float temp = 0.0f;
        int code = -1;
        String desc;  // 中文天气描述
    };

    struct Clock {
        bool valid = false;
        int hour = 0, min = 0, sec = 0;
        int year = 0, mon = 0, day = 0;
        int weekday = 0;  // 0=Sun .. 6=Sat
    };

    // 调用以更新天气/服务(按需阻塞, 受间隔限制)
    void update();

    // 本地时间(NTP epoch + tz 偏移)
    Clock localClock();

    // 当前天气(缓存)
    const Weather& weather();

    // 服务列表
    const ServiceStatus* services(int& count);

    // 初始化(读取配置)
    void begin();

    // 强制立即刷新天气(忽略间隔)
    void forceWeather();

    // 天气/服务描述缓存
    const char* weatherDesc();

    // WMO code -> 中文
    const char* codeToText(int code);
}  // namespace ClockWeather

#endif  // CLOCK_WEATHER_H
