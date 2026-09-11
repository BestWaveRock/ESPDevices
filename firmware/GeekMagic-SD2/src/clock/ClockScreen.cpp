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
    out->off = (uint16_t)(pgm_read_byte(b + 0) | (pgm_read_byte(b + 1) << 8));
    out->w = pgm_read_byte(b + 2);
    out->h = pgm_read_byte(b + 3);
    out->xoff = (int8_t)pgm_read_byte(b + 4);
    out->yoff = (int8_t)pgm_read_byte(b + 5);
    out->adv = pgm_read_byte(b + 6);
}

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

// s2: 半单位缩放参数 (2=1x, 1=0.5x, 3=1.5x, 4=2x). 目标像素 = 源像素 * s2 / 2 (最近邻)
static int16_t sscale(int16_t v, uint8_t s2) {
    return (int16_t)(((int32_t)v * s2 + 1) / 2);
}

// px 字号 -> 半单位缩放 (base 为字库原始像素高度)
static uint8_t scale2FromPx(int px, int base) {
    if (px < base / 2) px = base / 2;
    int s2 = (px * 2 + base / 2) / base;
    if (s2 < 1) s2 = 1;
    if (s2 > 4) s2 = 4;
    return (uint8_t)s2;
}

// px 字号 -> GLCD textSize (GLCD 基础 6x8, 8px/等级)
static uint8_t glcdFromPx(int px) {
    if (px < 8) px = 8;
    if (px > 24) px = 24;
    uint8_t ts = (uint8_t)((px + 4) / 8);
    if (ts < 1) ts = 1;
    if (ts > 3) ts = 3;
    return ts;
}

int16_t utf8Width(const char* s, uint8_t s2) {
    const char* p = s;
    int16_t w = 0;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            w += sscale(g.adv, s2);
        } else {
            w += sscale(12, s2);
        }
    }
    return w;
}

void drawUtf8(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color, uint8_t s2) {
    const char* p = s;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            if (g.w > 0 && g.h > 0) {
                if (s2 == 2) {
                    gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CJKBitmaps[g.off], g.w, g.h,
                                    color);
                } else {
                    // 最近邻缩放绘制 (s2=半单位, 目标: 源*s2/2)
                    int16_t nw = sscale(g.w, s2);
                    int16_t nh = sscale(g.h, s2);
                    int16_t nx = x + sscale(g.xoff, s2);
                    int16_t ny = baseline + sscale(g.yoff, s2);
                    uint8_t bw = (uint8_t)((g.w + 7) / 8);
                    for (int16_t dy = 0; dy < nh; dy++) {
                        int16_t sy = (int16_t)((dy * g.h) / nh);
                        const uint8_t* row = &CJKBitmaps[g.off + sy * bw];
                        for (int16_t dx = 0; dx < nw; dx++) {
                            int16_t sx = (int16_t)((dx * g.w) / nw);
                            if (pgm_read_byte(row + (sx >> 3)) & (0x80 >> (sx & 7))) gfx->writePixel(nx + dx, ny + dy, color);
                        }
                    }
                }
            }
            x += sscale(g.adv, s2);
        } else {
            x += sscale(12, s2);
        }
    }
}

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

// 秒级时钟: 逐数字小格子刷新, 只重绘变化的数字, 无整行黑闪.
// 时钟各数字 advance 固定 24px, 但字形像素范围不同 (xoff 4..9, w 9..22, h 32..34),
// 因此旧数字可能比新数字更宽/更高. 若按新数字尺寸清格, 旧数字右侧/底部会残留线条.
// 解法: 用固定满格清除框. 相邻数字像素间有 >=2px 间隙 (左邻最右到 x+2, 本位最左 x+4,
// 右邻最左 x+28), 清 x+3..x+26 (24px) 可完全覆盖旧数字而不误伤左右邻居.
// s2: 半单位缩放 (2=1x, 1=0.5x, 3=1.5x, 4=2x), 最近邻缩放
static void drawClockSeconds(Arduino_GFX* gfx, const char* s, const char* prev, int16_t baseline, uint16_t color,
                             uint8_t s2) {
    if (s2 < 1) s2 = 1;
    if (s2 > 4) s2 = 4;
    int16_t totalW = 0;
    {
        const char* pp = s;
        while (*pp != '\0') {
            int idx = clockFind(*pp);
            if (idx >= 0) {
                Glyph g;
                glyphAt(CLKMetrics, (uint16_t)idx, &g);
                totalW += sscale(g.adv, s2);
            } else {
                totalW += sscale(12, s2);
            }
            pp++;
        }
    }
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
                int16_t cw = sscale(24, s2);
                int16_t chh = sscale(36, s2);
                gfx->fillRect(x + sscale(3, s2), baseline - chh, cw, chh, LCD_BLACK);
                if (s2 == 2) {
                    gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CLKBitmaps[g.off], g.w, g.h,
                                    color);
                } else {
                    int16_t nw = sscale(g.w, s2);
                    int16_t nh = sscale(g.h, s2);
                    int16_t nx = x + sscale(g.xoff, s2);
                    int16_t ny = baseline + sscale(g.yoff, s2);
                    uint8_t bw = (uint8_t)((g.w + 7) / 8);
                    for (int16_t dy = 0; dy < nh; dy++) {
                        int16_t sy = (int16_t)((dy * g.h) / nh);
                        const uint8_t* row = &CLKBitmaps[g.off + sy * bw];
                        for (int16_t dx = 0; dx < nw; dx++) {
                            int16_t sx = (int16_t)((dx * g.w) / nw);
                            if (pgm_read_byte(row + (sx >> 3)) & (0x80 >> (sx & 7))) gfx->writePixel(nx + dx, ny + dy, color);
                        }
                    }
                }
            }
            x += sscale(g.adv, s2);
        } else {
            x += sscale(12, s2);
        }
        p++;
        q++;
    }
}

// 各分区独立行, 只在自身内容变化时清+重绘对应行, 整屏绝不再整区清 -> 无黑闪
static const int16_t IP_Y = 6;
static const int16_t CLOCK_BASELINE = 90;
static const int16_t DATE_TOP = 98;
static const int16_t LUNAR_BASELINE = 126;
static const int16_t WEATHER_BASELINE = 160;
static const char* WK_EN[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// 服务区: 每行1个服务, 两行/页, 超过则轮播.
// 服务区整体在 [天气行下方, 底部页点上方] 区间内垂直居中 (两行块总高=2*SVC_ROW_H)
static const int16_t SVC_BAND_TOP = 166;     // 天气行下方边界
static const int16_t SVC_BAND_BOTTOM = 232;   // 页点上方边界
static const int16_t SVC_ROW_H = 26;
static const int16_t SVC_GAP = 4;
static const int16_t SVC_TOP = (SVC_BAND_TOP + SVC_BAND_BOTTOM) / 2 - SVC_ROW_H;

// 延迟ms: <30ms绿, 30-99ms黄, >=100ms橙
static uint16_t svcDelayColor(int ms) {
    if (ms < 30) return (uint16_t)0x0A00;
    if (ms < 100) return (uint16_t)0x6540;
    return (uint16_t)0x4C40;
}

// 顶部: 左侧 GLCD 小字显示日期+星期, 右侧 GLCD 小字显示 IP
static void drawTopBar(Arduino_GFX* gfx, const char* dateStr, const char* ip) {
    uint8_t dfs = glcdFromPx(configManager.dateFontSize);
    uint8_t ifs = glcdFromPx(configManager.ipFontSize);
    gfx->setTextSize(dfs);
    gfx->setTextColor(0xB0B0B0, LCD_BLACK);
    gfx->setCursor(4, IP_Y);
    gfx->print(dateStr ? dateStr : "");

    if (ip && ip[0] != '\0') {
        int16_t pw = (int16_t)(strlen(ip) * 6 * ifs);
        gfx->setTextSize(ifs);
        gfx->setTextColor(0x608060, LCD_BLACK);
        gfx->setCursor(LCD_W - 4 - pw, IP_Y);
        gfx->print(ip);
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
    static bool lastSvcUp[8] = {false, false, false, false, false, false, false, false};
    static int lastLatMs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static unsigned long svcPageTs = 0;
    static int svcPage = 0;

    // ---- 本机 IP ----
    char ipBuf[16];
    String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("No Net");
    strncpy(ipBuf, ipStr.c_str(), sizeof(ipBuf) - 1);
    ipBuf[sizeof(ipBuf) - 1] = '\0';

    // ---- 日期行 ----
    char dateKey[32] = "";
    char dateStr[24] = "";
    if (clock.valid) {
        snprintf(dateKey, sizeof(dateKey), "%d-%02d-%02d-%d", clock.year, clock.mon, clock.day, clock.weekday);
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d %s", clock.year, clock.mon, clock.day, WK_EN[clock.weekday]);
    }
    bool topBarChanged = first || (strcmp(ipBuf, lastIp) != 0) || (clock.valid && strcmp(dateKey, lastDateKey) != 0);
    bool dateChanged = first || (clock.valid && strcmp(dateKey, lastDateKey) != 0);

    // ---- 天气行 ----
    char wlbuf[32];
    {
        const ClockWeather::Weather& w = ClockWeather::weather();
        if (w.ok) {
            snprintf(wlbuf, sizeof(wlbuf), "%s %.1f℃", ClockWeather::codeToText(w.code), w.temp);
        } else {
            snprintf(wlbuf, sizeof(wlbuf), "%s", "天气获取中...");
        }
    }
    const char* city = configManager.getCity();
    bool weatherChanged = first || (strcmp(wlbuf, lastWeather) != 0) || (strcmp(city, lastCity) != 0);

    // ---- 服务行 ----
    int n = 0;
    const ClockWeather::ServiceStatus* svcs = ClockWeather::services(n);

    if (first) {
        gfx->fillScreen(LCD_BLACK);
    }

    // 顶栏: 左日期+星期, 右 IP (合并独立区, 任一变化时重绘整区)
    if (topBarChanged) {
        uint8_t dfs = glcdFromPx(configManager.dateFontSize);
        uint8_t ifs = glcdFromPx(configManager.ipFontSize);
        int16_t h = dfs * 8 + 4;
        if (ifs * 8 + 4 > h) h = ifs * 8 + 4;
        if (h < 18) h = 18;
        gfx->fillRect(0, 0, LCD_W, h, LCD_BLACK);
        drawTopBar(gfx, dateStr, ipBuf);
    }

    // 时钟行 (秒级, 逐数字)
    uint8_t clkS2 = scale2FromPx(configManager.clockFontSize, 34);
    if (clock.valid) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", clock.hour, clock.min, clock.sec);
        if (first || !lastValid) {
            gfx->fillRect(0, CLOCK_BASELINE - sscale(36, clkS2) - 2, LCD_W, sscale(36, clkS2) + 2, LCD_BLACK);
        }
        drawClockSeconds(gfx, tbuf, lastTimeStr, CLOCK_BASELINE, LCD_WHITE, clkS2);
        strncpy(lastTimeStr, tbuf, sizeof(lastTimeStr) - 1);
        lastTimeStr[sizeof(lastTimeStr) - 1] = '\0';
    } else {
        gfx->fillRect(0, CLOCK_BASELINE - sscale(36, clkS2) - 2, LCD_W, sscale(36, clkS2) + 2, LCD_BLACK);
        static const char* t = "同步时间中...";
        drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, CLOCK_BASELINE, 0x608060);
        lastTimeStr[0] = '\0';
    }

    // 农历行 (独立, CJK 大字)
    if (dateChanged && clock.valid) {
        char lunarbuf[16];
        LunarCalendar::text(clock.year, clock.mon, clock.day, lunarbuf, sizeof(lunarbuf));
        if (lunarbuf[0] != '\0') {
            uint8_t lS2 = scale2FromPx(configManager.lunarFontSize, 16);
            int16_t lh = sscale(18, lS2) + 2;
            gfx->fillRect(0, LUNAR_BASELINE - lh, LCD_W, lh, LCD_BLACK);
            int16_t lw = utf8Width(lunarbuf, lS2);
            drawUtf8(gfx, lunarbuf, (LCD_W - lw) / 2, LUNAR_BASELINE, 0xD0D0D0, lS2);
        }
    }

    // 天气行: 区+天气+气温
    if (weatherChanged) {
        uint8_t wS2 = scale2FromPx(configManager.weatherFontSize, 16);
        int16_t wh = sscale(20, wS2) + 2;
        gfx->fillRect(0, WEATHER_BASELINE - wh, LCD_W, wh, LCD_BLACK);
        const char* district = city;  // city 完整显示, 不再截取
        int16_t dw = utf8Width(district, wS2);
        int16_t ww = utf8Width(wlbuf, wS2);
        int16_t sx = (LCD_W - (dw + sscale(8, wS2) + ww)) / 2;
        drawUtf8(gfx, district, sx, WEATHER_BASELINE, 0x90E090, wS2);
        drawUtf8(gfx, wlbuf, sx + dw + sscale(8, wS2), WEATHER_BASELINE, LCD_WHITE, wS2);
    }

    // 服务行 (单列定宽: 状态点 + 定宽地址 + 空6px + 延迟ms; 两行/页, 每10秒轮播)
    {
        unsigned long now = millis();
        if (svcPageTs == 0) svcPageTs = now;
        if (now - svcPageTs >= 10000UL) {
            svcPageTs = now;
            if (n > 2) {
                int totalPages = (n + 1) / 2;
                svcPage = (svcPage + 1) % totalPages;
            }
        }
        bool svcChanged = first;
        for (int i = 0; i < n && i < 8; i++) {
            if (svcs[i].up != lastSvcUp[i] || svcs[i].latency_ms != lastLatMs[i]) { svcChanged = true; break; }
        }
        if (svcChanged) {
            gfx->fillRect(0, SVC_TOP - 2, LCD_W, LCD_H - SVC_TOP + 2, LCD_BLACK);
            if (n == 0) {
                static const char* t = "未配置监控服务";
                drawUtf8(gfx, t, (LCD_W - utf8Width(t)) / 2, SVC_TOP + 20, 0x606060);
            } else {
                uint8_t svcFs = glcdFromPx(configManager.serviceFontSize);
                int16_t addrW = svcFs * 96 + 40;   // 地址定宽: size1=136, size2=232, size3=328
                int pageStart = svcPage * 2;
                int rowsOnPage = (n - pageStart >= 2) ? 2 : (n - pageStart);
                int16_t textH = 8 * svcFs;
                int16_t yOff = (SVC_ROW_H - textH) / 2;   // 文本在行高内垂直居中
                for (int r = 0; r < rowsOnPage; r++) {
                    int i = pageStart + r;
                    if (i >= n || i >= 8) break;
                    int16_t x = 8;
                    int16_t y = SVC_TOP + r * SVC_ROW_H + yOff;
                    uint16_t dotColor = svcs[i].up ? LCD_GREEN : LCD_RED;
                    gfx->fillCircle(x + 3, y + textH / 2, 3, dotColor);
                    char lbl[48];
                    snprintf(lbl, sizeof(lbl), "%s:%d", svcs[i].ip.c_str(), svcs[i].port);
                    gfx->setTextSize(svcFs);
                    gfx->setTextColor(0xD0D0D0, LCD_BLACK);
                    gfx->setCursor(x + 10, y);
                    gfx->print(lbl);
                    if (svcs[i].up) {
                        gfx->setTextSize(svcFs);
                        gfx->setTextColor(svcDelayColor(svcs[i].latency_ms), LCD_BLACK);
                        gfx->setCursor(x + addrW + SVC_GAP, y);
                        gfx->printf("%dms", svcs[i].latency_ms);
                    } else {
                        gfx->setTextSize(svcFs);
                        gfx->setTextColor(0xF08080, LCD_BLACK);
                        gfx->setCursor(x + addrW + SVC_GAP, y);
                        gfx->print("down");
                    }
                }
                // 页码点 (超过2个服务时显示)
                if (n > 2) {
                    int totalPages = (n + 1) / 2;
                    for (int p = 0; p < totalPages; p++) {
                        int px = LCD_W / 2 - (totalPages * 6) / 2 + p * 6;
                        gfx->fillCircle(px, LCD_H - 4, 2, (p == svcPage) ? LCD_WHITE : 0x404040);
                    }
                }
            }
        }
        for (int i = 0; i < 8; i++) {
            lastSvcUp[i] = (i < n) ? svcs[i].up : false;
            lastLatMs[i] = (i < n) ? svcs[i].latency_ms : 0;
        }
        lastSvcCount = n;
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
    first = false;

    yield();
}

}  // namespace ClockScreen