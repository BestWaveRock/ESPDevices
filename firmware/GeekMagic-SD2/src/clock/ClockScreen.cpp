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
#include <ESP8266WiFi.h>

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

// 各分区独立行, 只在自身内容变化时清+重绘对应行, 整屏绝不再整区清 -> 无黑闪
static const int16_t IP_Y = 6;
static const int16_t CLOCK_BASELINE = 90;
static const int16_t DATE_TOP = 98;      // 日期+星期 (GLCD 小字, y 为顶部)
static const int16_t LUNAR_BASELINE = 126;  // 农历 (CJK 大字, y 为基线)
static const int16_t WEATHER_BASELINE = 160;
static const char* WK_EN[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// 顶部用内置 GLCD 字体展示本机 IP (ASCII, size1 每字符约 6px)
static void drawIp(Arduino_GFX* gfx, const char* ip) {
    if (ip == nullptr || ip[0] == '\0') return;
    int w = (int)strlen(ip) * 6;
    gfx->setTextSize(1);
    gfx->setTextColor(0x608060, LCD_BLACK);
    gfx->setCursor((LCD_W - w) / 2, IP_Y);
    gfx->print(ip);
}

// 秒级时钟: 逐数字小格子刷新, 只重绘变化的数字, 无整行黑闪.
// 时钟各数字 advance 固定 24px, 但字形像素范围不同 (xoff 4..9, w 9..22, h 32..34),
// 因此旧数字可能比新数字更宽/更高. 若按新数字尺寸清格, 旧数字右侧/底部会残留线条.
// 解法: 用固定满格清除框. 相邻数字像素间有 >=2px 间隙 (左邻最右到 x+2, 本位最左 x+4,
// 右邻最左 x+28), 清 x+3..x+26 (24px) 可完全覆盖旧数字而不误伤左右邻居.
static void drawClockSeconds(Arduino_GFX* gfx, const char* s, const char* prev, int16_t baseline, uint16_t color) {
    int16_t totalW = clockWidth(s);
    int16_t x = (LCD_W - totalW) / 2;
    const char* p = s;
    const char* q = prev;
    while (*p != '\0') {
        bool changed = (*p != *q);
        int idx = clockFind(*p);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CLKMetrics, (uint16_t)idx, &g);
            if (changed && g.w > 0 && g.h > 0) {
                // 固定满格清除: 覆盖任意前驱数字的完整像素范围, 高度取数字最大 34+余量
                gfx->fillRect(x + 3, baseline - 36, 24, 36, LCD_BLACK);
                gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CLKBitmaps[g.off], g.w, g.h, color);
            }
            x += g.adv;
        } else {
            x += 12;
        }
        p++;
        q++;
    }
}

void render(Arduino_GFX* gfx) {
    auto clock = ClockWeather::localClock();

    static bool first = true;
    static bool lastValid = false;
    static char lastTimeStr[16] = "";
    static char lastDateKey[32] = "";
    static char lastWeather[32] = "";
    static char lastCity[32] = "";
    static char lastIp[16] = "";
    static int lastSvcCount = -1;
    static bool lastSvcUp[4] = {false, false, false, false};

    // ---- 本机 IP ----
    char ipBuf[16];
    String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("No Net");
    strncpy(ipBuf, ipStr.c_str(), sizeof(ipBuf) - 1);
    ipBuf[sizeof(ipBuf) - 1] = '\0';
    bool ipChanged = first || strcmp(ipBuf, lastIp) != 0;

    // ---- 日期行 ----
    char dateKey[32] = "";
    if (clock.valid) {
        snprintf(dateKey, sizeof(dateKey), "%d-%02d-%02d-%d", clock.year, clock.mon, clock.day, clock.weekday);
    }
    bool dateChanged = first || (clock.valid && strcmp(dateKey, lastDateKey) != 0);

    // ---- 天气行 ----
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

    // ---- 服务行 ----
    int n = 0;
    const ClockWeather::ServiceStatus* svcs = ClockWeather::services(n);
    bool svcChanged = (n != lastSvcCount);
    for (int i = 0; i < n && i < 4; i++) {
        if (svcs[i].up != lastSvcUp[i]) { svcChanged = true; break; }
    }
    svcChanged = svcChanged || first;

    if (first) {
        gfx->fillScreen(LCD_BLACK);
    }

    // IP 行 (独立)
    if (ipChanged) {
        gfx->fillRect(0, 0, LCD_W, 20, LCD_BLACK);
        drawIp(gfx, ipBuf);
    }

    // 时钟行 (秒级, 逐数字)
    if (clock.valid) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", clock.hour, clock.min, clock.sec);
        if (first || !lastValid) {
            gfx->fillRect(0, 48, LCD_W, 48, LCD_BLACK);  // 状态切换, 清一次
        }
        drawClockSeconds(gfx, tbuf, lastTimeStr, CLOCK_BASELINE, LCD_WHITE);
        strncpy(lastTimeStr, tbuf, sizeof(lastTimeStr) - 1);
        lastTimeStr[sizeof(lastTimeStr) - 1] = '\0';
    } else {
        gfx->fillRect(0, 48, LCD_W, 48, LCD_BLACK);
        static const char* t = "同步时间中...";
        drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, CLOCK_BASELINE, 0x608060);
        lastTimeStr[0] = '\0';
    }

    // 日期+星期行 (独立, GLCD 小字, 英文星期避免字体缺"周")
    if (dateChanged && clock.valid) {
        char line[24];
        snprintf(line, sizeof(line), "%04d-%02d-%02d %s", clock.year, clock.mon, clock.day, WK_EN[clock.weekday]);
        gfx->fillRect(0, DATE_TOP - 1, LCD_W, 12, LCD_BLACK);
        int16_t lw = (int16_t)(strlen(line) * 6);
        gfx->setTextSize(1);
        gfx->setTextColor(0xC0C0C0, LCD_BLACK);
        gfx->setCursor((LCD_W - lw) / 2, DATE_TOP);
        gfx->print(line);
    }

    // 农历行 (独立, CJK 大字)
    if (dateChanged && clock.valid) {
        char lunarbuf[16];
        LunarCalendar::text(clock.year, clock.mon, clock.day, lunarbuf, sizeof(lunarbuf));
        if (lunarbuf[0] != '\0') {
            gfx->fillRect(0, 110, LCD_W, 20, LCD_BLACK);
            int16_t lw = utf8Width(lunarbuf);
            drawUtf8(gfx, lunarbuf, (LCD_W - lw) / 2, LUNAR_BASELINE, 0xD0D0D0);
        }
    }

    // 天气行 (独立)
    if (weatherChanged) {
        gfx->fillRect(0, 138, LCD_W, 28, LCD_BLACK);
        int16_t cw = utf8Width(city);
        int16_t ww = utf8Width(wlbuf);
        int16_t sx = (LCD_W - (cw + 10 + ww)) / 2;
        drawUtf8(gfx, city, sx, WEATHER_BASELINE, 0x90E090);
        drawUtf8(gfx, wlbuf, sx + cw + 10, WEATHER_BASELINE, LCD_WHITE);
    }

    // 服务行 (独立)
    if (svcChanged) {
        gfx->fillRect(0, 195, LCD_W, LCD_H - 195, LCD_BLACK);
        for (int i = 0; i < n && i < 4; i++) {
            int col = i % 2;
            int row = i / 2;
            int16_t x = (col == 0) ? 8 : 124;
            int16_t y = 206 + row * 22;
            gfx->fillCircle(x, y - 4, 3, svcs[i].up ? LCD_GREEN : LCD_RED);
            const char* sip = svcs[i].ip.c_str();
            drawUtf8(gfx, sip, x + 8, y, 0xB0B0B0);
            int16_t ipw = utf8Width(sip);
            drawUtf8(gfx, svcs[i].up ? "在线" : "离线", x + 8 + ipw + 4, y,
                     svcs[i].up ? 0x90E090 : 0xF08080);
        }
        if (n == 0) {
            static const char* t = "未配置监控服务";
            drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, 210, 0x606060);
        }
    }

    // 更新跟踪状态
    lastValid = clock.valid;
    strncpy(lastDateKey, dateKey, sizeof(lastDateKey) - 1);
    lastDateKey[sizeof(lastDateKey) - 1] = '\0';
    strncpy(lastWeather, wlbuf, sizeof(lastWeather) - 1);
    lastWeather[sizeof(lastWeather) - 1] = '\0';
    strncpy(lastCity, city, sizeof(lastCity) - 1);
    lastCity[sizeof(lastCity) - 1] = '\0';
    strncpy(lastIp, ipBuf, sizeof(lastIp) - 1);
    lastIp[sizeof(lastIp) - 1] = '\0';
    lastSvcCount = n;
    for (int i = 0; i < 4; i++) lastSvcUp[i] = (i < n) ? svcs[i].up : false;
    first = false;

    yield();
}

}  // namespace ClockScreen
