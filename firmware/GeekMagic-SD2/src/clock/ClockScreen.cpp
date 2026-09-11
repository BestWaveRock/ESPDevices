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

#include "clock/ClockScreen.h"
#include "display/DisplayManager.h"
#include "clock/ClockWeather.h"
#include "lunar/LunarCalendar.h"
#include "config/ConfigManager.h"
#include "fonts/cjk_font.h"
#include <string.h>
#include <stddef.h>
#include <pgmspace.h>

extern ConfigManager configManager;

namespace ClockScreen {

// Font/metrics live in flash (.irom.text). ESP8266 flash faults on unaligned
// 8/16-bit loads, so every field is read with pgm_read_byte (which emits a safe
// word-load + shift). We build the uint16 from two bytes and write to an out
// param (no struct-return) so the compiler cannot substitute a direct flash load.
static void glyphAt(const Glyph* arr, uint16_t i, Glyph* out) {
    const uint8_t* b = (const uint8_t*)arr + (unsigned)i * sizeof(Glyph);
    // off 是 uint16_t, ESP8266 小端: byte0=低字节, byte1=高字节
    out->off = (uint16_t)(pgm_read_byte(b + 0) | (pgm_read_byte(b + 1) << 8));
    out->w = pgm_read_byte(b + 2);
    out->h = pgm_read_byte(b + 3);
    out->xoff = (int8_t)pgm_read_byte(b + 4);
    out->yoff = (int8_t)pgm_read_byte(b + 5);
    out->adv = pgm_read_byte(b + 6);
}

// ---- UTF-8 解码: 返回码点并前进指针 ----
static uint32_t decodeUtf8(const char*& p) {
    unsigned char c = *p++;
    if (c < 0x80) return c;
    if (c < 0xE0) {
        unsigned char c2 = *p++;
        return ((c & 0x1F) << 6) | (c2 & 0x3F);
    }
    if (c < 0xF0) {
        unsigned char c2 = *p++;
        unsigned char c3 = *p++;
        return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    p += 2;
    return 0;
}

static int findGlyph(const uint32_t* cps, uint16_t count, uint32_t cp) {
    for (uint16_t i = 0; i < count; i++) {
        if (pgm_read_dword(&cps[i]) == cp) return i;
    }
    return -1;
}

int16_t utf8Width(const char* s) {
    const char* p = s;
    int16_t w = 0;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            w += g.adv;
        } else {
            w += 12;
        }
    }
    return w;
}

void drawUtf8(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color) {
    const char* p = s;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            if (g.w > 0 && g.h > 0) {
                gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CJKBitmaps[g.off], g.w, g.h, color);
            }
            x += g.adv;
        } else {
            x += 12;
        }
    }
}

// 显式线性查找, 避免编译器为 switch 生成跳转表 (ESP8266 坑)
static int clockFind(char c) {
    int n = CLKCount;
    for (int i = 0; i < n; i++) {
        uint32_t cp = pgm_read_dword(&CLKCodepoints[i]);
        if (cp == (unsigned char)c) return i;
    }
    return -1;
}

int16_t clockWidth(const char* s) {
    int16_t w = 0;
    const char* p = s;
    while (*p != '\0') {
        int idx = clockFind(*p);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CLKMetrics, (uint16_t)idx, &g);
            w += g.adv;
        } else {
            w += 12;
        }
        p++;
    }
    return w;
}

void drawClock(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color) {
    const char* p = s;
    while (*p != '\0') {
        int idx = clockFind(*p);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CLKMetrics, (uint16_t)idx, &g);
            if (g.w > 0 && g.h > 0) {
                gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CLKBitmaps[g.off], g.w, g.h, color);
            }
            x += g.adv;
        } else {
            x += 12;
        }
        p++;
    }
}

// 增量刷新: 只在内容变化时重绘对应区域, 避免每秒整屏黑闪
static const uint16_t SEC_CLOCK_BG = 0x0A1A;
static const uint16_t SEC_WEATHER_BG = 0x0E22;
static const uint16_t SEC_SERVICE_BG = 0x0A1A;

void render(Arduino_GFX* gfx) {
    auto clock = ClockWeather::localClock();

    static bool first = true;
    static int lastH = -1, lastM = -1;
    static int lastY = -1, lastMo = -1, lastD = -1;
    static bool lastValid = false;
    static char lastWeather[32] = "";
    static char lastCity[32] = "";
    static int lastSvcCount = -1;
    static bool lastSvcUp[4] = {false, false, false, false};

    bool clockChanged = first || (!clock.valid != !lastValid) || (clock.hour != lastH) || (clock.min != lastM);
    bool dateChanged = clockChanged && (clock.valid && (clock.year != lastY || clock.mon != lastMo || clock.day != lastD));

    char wlbuf[32];
    {
        const ClockWeather::Weather& w = ClockWeather::weather();
        if (w.ok) {
            snprintf(wlbuf, sizeof(wlbuf), "%s %d℃", ClockWeather::codeToText(w.code), (int)w.temp);
        } else {
            snprintf(wlbuf, sizeof(wlbuf), "%s", "天气获取中...");
        }
    }
    const char* city = configManager.getCity();
    bool weatherChanged = first || (strcmp(wlbuf, lastWeather) != 0) || (strcmp(city, lastCity) != 0);

    int n = 0;
    const ClockWeather::ServiceStatus* svcs = ClockWeather::services(n);
    bool svcChanged = false;
    if (n != lastSvcCount) svcChanged = true;
    for (int i = 0; i < n && i < 4; i++) {
        if (svcs[i].up != lastSvcUp[i]) { svcChanged = true; break; }
    }
    svcChanged = svcChanged || first;

    if (first) {
        gfx->fillScreen(LCD_BLACK);
        gfx->fillRect(0, 0, LCD_W, 130, SEC_CLOCK_BG);
        gfx->fillRect(0, 130, LCD_W, 60, SEC_WEATHER_BG);
        gfx->fillRect(0, 190, LCD_W, 50, SEC_SERVICE_BG);
    }

    if (clockChanged || dateChanged) {
        gfx->fillRect(0, 0, LCD_W, 130, SEC_CLOCK_BG);
        if (clock.valid) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", clock.hour, clock.min);
            int16_t w = clockWidth(buf);
            drawClock(gfx, buf, (LCD_W - w) / 2, 90, LCD_WHITE);
        } else {
            static const char* t = "同步时间中...";
            drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, 90, 0x608060);
        }
    }

    if (dateChanged) {
        if (clock.valid) {
            static const char* WK[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
            char lunarbuf[16];
            LunarCalendar::text(clock.year, clock.mon, clock.day, lunarbuf, sizeof(lunarbuf));
            char line[64];
            if (lunarbuf[0] != '\0') {
                snprintf(line, sizeof(line), "%d-%02d-%02d %s %s", clock.year, clock.mon, clock.day, WK[clock.weekday], lunarbuf);
            } else {
                snprintf(line, sizeof(line), "%d-%02d-%02d %s", clock.year, clock.mon, clock.day, WK[clock.weekday]);
            }
            int16_t lw = utf8Width(line);
            drawUtf8(gfx, line, (LCD_W - lw) / 2, 122, 0xC0C0C0);
        }
    }

    if (weatherChanged) {
        gfx->fillRect(0, 130, LCD_W, 60, SEC_WEATHER_BG);
        int16_t yw = 160;
        int16_t cw = utf8Width(city);
        int16_t ww = utf8Width(wlbuf);
        int16_t sx = (LCD_W - (cw + 10 + ww)) / 2;
        drawUtf8(gfx, city, sx, yw, 0x90E090);
        drawUtf8(gfx, wlbuf, sx + cw + 10, yw, LCD_WHITE);
    }

    if (svcChanged) {
        gfx->fillRect(0, 190, LCD_W, 50, SEC_SERVICE_BG);
        for (int i = 0; i < n && i < 4; i++) {
            int col = i % 2;
            int row = i / 2;
            int16_t x = (col == 0) ? 8 : 124;
            int16_t y = 206 + row * 22;
            gfx->fillCircle(x, y - 4, 3, svcs[i].up ? LCD_GREEN : LCD_RED);
            const char* ip = svcs[i].ip.c_str();
            drawUtf8(gfx, ip, x + 8, y, 0xB0B0B0);
            int16_t ipw = utf8Width(ip);
            drawUtf8(gfx, svcs[i].up ? "在线" : "离线", x + 8 + ipw + 4, y,
                     svcs[i].up ? 0x90E090 : 0xF08080);
        }
        if (n == 0) {
            static const char* t = "未配置监控服务";
            drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, 210, 0x606060);
        }
    }

    // 更新跟踪状态
    lastH = clock.hour;
    lastM = clock.min;
    lastY = clock.year;
    lastMo = clock.mon;
    lastD = clock.day;
    lastValid = clock.valid;
    strncpy(lastWeather, wlbuf, sizeof(lastWeather) - 1);
    lastWeather[sizeof(lastWeather) - 1] = '\0';
    strncpy(lastCity, city, sizeof(lastCity) - 1);
    lastCity[sizeof(lastCity) - 1] = '\0';
    lastSvcCount = n;
    for (int i = 0; i < 4; i++) lastSvcUp[i] = (i < n) ? svcs[i].up : false;
    first = false;

    yield();
}

}  // namespace ClockScreen
