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

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <ArduinoJson.h>
#include "config/SecureStorage.h"
#include <string>
#include <cstdint>
#include <SPI.h>

// LCD configuration defaults for hellocubic lite
static constexpr int16_t LCD_W = 240;
static constexpr int16_t LCD_H = 240;
static constexpr uint8_t LCD_ROTATION = 4;
static constexpr int8_t LCD_MOSI_GPIO = 13;
static constexpr int8_t LCD_SCK_GPIO = 14;
static constexpr int8_t LCD_CS_GPIO = 15;
static constexpr int8_t LCD_DC_GPIO = 0;
static constexpr int8_t LCD_RST_GPIO = 2;
static constexpr uint8_t LCD_SPI_MODE = SPI_MODE3;
static constexpr uint32_t LCD_SPI_HZ = 40000000;
static constexpr int8_t LCD_BACKLIGHT_GPIO = 5;
static constexpr bool LCD_BACKLIGHT_ACTIVE_LOW = true;

class ConfigManager {
   public:
    ConfigManager(const char* filename = "/config.json");
    bool load();
    bool save();
    void setWiFi(const char* newSsid, const char* newPassword);
    const char* getSSID() const;
    const char* getPassword() const;
    const char* getApiToken() const;
    void setApiToken(const char* newApiToken);
    uint8_t getLCDRotation() const;
    void setLCDRotation(uint8_t newRotation);
    uint32_t getLCDSpiHz() const;

   public:
    uint8_t getLCDRotationSafe() const { return lcd_rotation; }
    std::string ssid;
    std::string password;
    std::string api_token;
    std::string filename;
    SecureStorage secure;
    uint8_t lcd_rotation = 4;
    std::string ntp_server;

    const char* getNtpServer() const { return ntp_server.c_str(); }
    void setNtpServer(const char* s) {
        if (s) ntp_server = s;
    }

    // Clock / weather / service monitor config
    float lat = 29.65f;
    float lng = 106.55f;
    std::string city = "重庆·两江新区";
    std::string services = "192.168.3.1:80";   // comma list "ip:port,ip:port"
    int weather_min = 15;                        // 天气刷新(分钟)
    int service_sec = 10;                        // 服务探测(秒)
    int tz_offset = 8;                           // 时区(小时)

    const char* getCity() const { return city.c_str(); }
    const char* getServices() const { return services.c_str(); }
    void setWeatherCity(const char* c, float la, float ln) {
        if (c) city = c;
        lat = la;
        lng = ln;
    }
    void setServices(const char* s) {
        if (s) services = s;
    }
};

#endif  // CONFIG_MANAGER_H
