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
#include "fonts/glcd5x7.h"
#include <string.h>
#include <stddef.h>
#include <pgmspace.h>
#include <math.h>
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

// 定点缩放: scale256 每倍=256 (范围 0.5x..4x). 目标像素 = v*scale256/256 (最近邻)
static int16_t sscale(int16_t v, uint16_t scale256) {
    return (int16_t)(((int32_t)v * scale256 + 128) >> 8);
}

// px 字号 -> 定点缩放比例 (base 为字库原始像素高度). 真实 px 渲染,
// 不再折算成离散档位: px 即渲染高度.
static uint16_t scaleFromPx(int px, int base) {
    if (base <= 0) base = 1;
    if (px < base / 2) px = base / 2;
    uint32_t s = ((uint32_t)px << 8) / (uint32_t)base;
    if (s < 128) s = 128;    // 最小 0.5x
    if (s > 1024) s = 1024;  // 最大 4x
    return (uint16_t)s;
}

int16_t utf8Width(const char* s, uint16_t scale256) {
    const char* p = s;
    int16_t w = 0;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            w += sscale(g.adv, scale256);
        } else {
            w += sscale(12, scale256);
        }
    }
    return w;
}

void drawUtf8(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color, uint16_t scale256) {
    const char* p = s;
    while (*p) {
        uint32_t cp = decodeUtf8(p);
        int idx = findGlyph(CJKCodepoints, CJKCount, cp);
        if (idx >= 0) {
            Glyph g;
            glyphAt(CJKMetrics, (uint16_t)idx, &g);
            if (g.w > 0 && g.h > 0) {
                if (scale256 == 256) {
                    gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CJKBitmaps[g.off], g.w, g.h,
                                    color);
                } else {
                    // 最近邻缩放绘制 (目标: 源*scale256/256)
                    int16_t nw = sscale(g.w, scale256);
                    int16_t nh = sscale(g.h, scale256);
                    int16_t nx = x + sscale(g.xoff, scale256);
                    int16_t ny = baseline + sscale(g.yoff, scale256);
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
            x += sscale(g.adv, scale256);
        } else {
            x += sscale(12, scale256);
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
// scale256: 定点缩放比例 (256=1x, 128=0.5x, 384=1.5x, 512=2x), 最近邻缩放
static void drawClockSeconds(Arduino_GFX* gfx, const char* s, const char* prev, int16_t baseline, uint16_t color,
                             uint16_t scale256) {
    if (scale256 < 128) scale256 = 128;
    if (scale256 > 1024) scale256 = 1024;
    int16_t totalW = 0;
    {
        const char* pp = s;
        while (*pp != '\0') {
            int idx = clockFind(*pp);
            if (idx >= 0) {
                Glyph g;
                glyphAt(CLKMetrics, (uint16_t)idx, &g);
                totalW += sscale(g.adv, scale256);
            } else {
                totalW += sscale(12, scale256);
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
                int16_t cw = sscale(24, scale256);
                int16_t chh = sscale(36, scale256);
                gfx->fillRect(x + sscale(3, scale256), baseline - chh, cw, chh, LCD_BLACK);
                if (scale256 == 256) {
                    gfx->drawBitmap((int16_t)(x + g.xoff), (int16_t)(baseline + g.yoff), &CLKBitmaps[g.off], g.w, g.h,
                                    color);
                } else {
                    int16_t nw = sscale(g.w, scale256);
                    int16_t nh = sscale(g.h, scale256);
                    int16_t nx = x + sscale(g.xoff, scale256);
                    int16_t ny = baseline + sscale(g.yoff, scale256);
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
            x += sscale(g.adv, scale256);
        } else {
            x += sscale(12, scale256);
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

// 主题配色: 影响文字/图形可选色. 10 套; configManager.theme 选择.
struct ThemePalette {
    uint16_t topBarDate;   // 顶栏日期+星期
    uint16_t ip;           // 顶栏 IP
    uint16_t clock;        // 时钟数字
    uint16_t sync;         // 同步时间中...
    uint16_t lunar;        // 农历
    uint16_t weatherArea;  // 天气地区
    uint16_t weatherInfo;  // 天气信息(温度+描述)
    uint16_t pie;          // 天气刷新饼图
    uint16_t svcNone;      // 未配置监控服务
    uint16_t svcAddr;      // 服务地址
};

static const ThemePalette THEMES[10] = {
    // 0 Green (默认)
    {0xAD75, 0x640C, LCD_WHITE, 0x640C, 0xCE79, 0x96F2, LCD_WHITE, 0x3499, 0x630C, 0xCE79},
    // 1 Blue
    {0x9DFB, 0x4418, LCD_WHITE, 0x4418, 0xC6DD, 0x651D, 0xE79F, 0x2399, 0x53D3, 0xC6DD},
    // 2 Purple
    {0xB55A, 0x9297, LCD_WHITE, 0x9297, 0xD65D, 0x9B9C, 0xEF5F, 0x7195, 0x624F, 0xD65D},
    // 3 Cyan
    {0x9EBA, 0x3513, LCD_WHITE, 0x3513, 0xC77D, 0x569A, 0xE7FF, 0x1451, 0x43CF, 0xC77D},
    // 4 Amber
    {0xEEB3, 0xBC46, LCD_WHITE, 0xBC46, 0xF738, 0xED48, 0xFFBA, 0xAB84, 0x8B89, 0xF738},
    // 5 Red
    {0xED73, 0xBA46, LCD_WHITE, 0xBA46, 0xF678, 0xE34A, 0xFF7D, 0xA984, 0x8A48, 0xF678},
    // 6 Pink
    {0xEDF9, 0xCB52, LCD_WHITE, 0xCB52, 0xF6BC, 0xEC14, 0xFF7E, 0xBA8F, 0x8B0E, 0xF6BC},
    // 7 Orange
    {0xF6B5, 0xD406, LCD_WHITE, 0xD406, 0xF738, 0xF4C7, 0xFF9B, 0xC343, 0x8B49, 0xF6B5},
    // 8 White
    {0xBDF7, 0x8410, LCD_WHITE, 0x8410, 0xD6BA, 0x9D13, LCD_WHITE, 0x8C52, 0x738E, 0xD6BA},
    // 9 Yellow
    {0xE6F3, 0xAD04, LCD_WHITE, 0xAD04, 0xEF58, 0xCDE6, 0xFFD8, 0x9402, 0x83C7, 0xEF58},
};

static const ThemePalette& currentTheme() {
    int t = configManager.theme;
    if (t < 0) t = 0;
    if (t > 9) t = 9;
    return THEMES[t];
}

// 延迟ms: <30绿, 30-99黄, 100-299橙, >=300深橙(红)
static uint16_t svcDelayColor(int ms) {
    if (ms < 30) return (uint16_t)0x0A00;
    if (ms < 100) return (uint16_t)0x9C40;
    if (ms < 300) return (uint16_t)0x19E0;
    return (uint16_t)0xF800;
}

// GLCD 5x7 字体: px 任意缩放绘制 (真实 px, 非档位). scale256=px*256/8.
// 字形 5 列 x 8 行 (第8行存 g/y/p/q 等下伸部分, LSB 为顶行), 字符等宽 6 列.
// reverseMap=true: 反向映射 (目标像素 -> 源列/行), 与 CJK 缩放一致, 非整数倍缩放无列空洞/黑线;
// reverseMap=false: 正向映射 (源列/行 -> 目标像素), 非整数倍缩放的列取整可能留空隙.
static void drawGlcd(Arduino_GFX* gfx, const char* s, int16_t x, int16_t y, uint16_t color, uint16_t scale256) {
    if (scale256 < 128) scale256 = 128;
    if (scale256 > 1024) scale256 = 1024;
    const char* p = s;
    int16_t cx = x;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) { p++; continue; }
        if (c > 0x7E) c = '?';
        const uint8_t* cols = &GLCD5x7[(unsigned)(c - 0x20) * 5];
        int16_t nw = sscale(6, scale256);   // 字符格宽
        if (configManager.reverseMap) {
            int16_t gw = sscale(5, scale256);   // 字形区宽
            int16_t gh = sscale(8, scale256);   // 字形区高 (含下伸行)
            for (int16_t dy = 0; dy < gh; dy++) {
                int16_t sy = (int16_t)((dy * 8) / gh);   // 源行 0..7
                uint8_t mask = (uint8_t)(1 << sy);
                for (int16_t dx = 0; dx < gw; dx++) {
                    int16_t sx = (int16_t)((dx * 5) / gw);  // 源列
                    if (pgm_read_byte(&cols[sx]) & mask) gfx->writePixel(cx + dx, y + dy, color);
                }
            }
        } else {
            for (int16_t i = 0; i < 5; i++) {
                uint8_t bits = pgm_read_byte(&cols[i]);
                for (int16_t sy = 0; sy < 8; sy++) {
                    if (bits & (1 << sy)) {
                        gfx->writePixel(cx + sscale(i, scale256), y + sscale(sy, scale256), color);
                    }
                }
            }
        }
        cx += nw;
        p++;
    }
}

// 天气刷新倒计时饼图: 纯圆填充区, 直径=行高, 从 12 点顺时针绘制剩余进度
// frac=1.0 整圆 (刚刷新), frac=0.0 空 (即将刷新). 用多边形小弧块近似扇形.
static void drawPie(Arduino_GFX* gfx, int16_t cx, int16_t cy, int16_t r, float frac, uint16_t color) {
    if (r <= 0) return;
    if (frac >= 1.0f) {
        gfx->fillCircle(cx, cy, r, color);
        return;
    }
    if (frac <= 0.0f) return;
    const int steps = 16;                       // 整圆分段数(每段 22.5°)
    float end = frac * (float)M_PI * 2.0f;
    int n = (int)(frac * steps) + 1;
    float prevX = cx;
    float prevY = cy - r;                       // 12 点方向
    for (int i = 1; i <= n; i++) {
        float a = end * (float)i / (float)n;
        float nx = cx - r * sinf(a);            // 逆时针: 从12点向左扫 (视觉上顺时针)
        float ny = cy - r * cosf(a);
        gfx->fillTriangle(cx, cy, (int16_t)prevX, (int16_t)prevY, (int16_t)nx, (int16_t)ny, color);
        prevX = nx;
        prevY = ny;
    }
}

// 顶部: 左侧 GLCD 小字显示日期+星期, 右侧 GLCD 小字显示 IP
static void drawTopBar(Arduino_GFX* gfx, const char* dateStr, const char* ip) {
    const ThemePalette& th = currentTheme();
    uint16_t dfs = scaleFromPx(configManager.dateFontSize, 8);
    uint16_t ifs = scaleFromPx(configManager.ipFontSize, 8);
    drawGlcd(gfx, dateStr ? dateStr : "", 4, IP_Y, th.topBarDate, dfs);

    if (ip && ip[0] != '\0') {
        int16_t pw = sscale((int16_t)strlen(ip) * 6, ifs);
        drawGlcd(gfx, ip, LCD_W - 4 - pw, IP_Y, th.ip, ifs);
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
    static int lastPiePct = -1;

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

    // 日期行高度 (顶栏与天气饼图共用)
    uint16_t dfs = scaleFromPx(configManager.dateFontSize, 8);
    uint16_t ifs = scaleFromPx(configManager.ipFontSize, 8);
    int16_t dateBarH = sscale(8, dfs) + 4;
    if (sscale(8, ifs) + 4 > dateBarH) dateBarH = sscale(8, ifs) + 4;
    if (dateBarH < 18) dateBarH = 18;

    // 顶栏: 左日期+星期, 右 IP (合并独立区, 任一变化时重绘整区)
    if (topBarChanged) {
        gfx->fillRect(0, 0, LCD_W, dateBarH, LCD_BLACK);
        drawTopBar(gfx, dateStr, ipBuf);
    }

    // 时钟行 (秒级, 逐数字)
    uint16_t clkScale = scaleFromPx(configManager.clockFontSize, 34);
    if (clock.valid) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", clock.hour, clock.min, clock.sec);
        if (first || !lastValid) {
            gfx->fillRect(0, CLOCK_BASELINE - sscale(36, clkScale) - 2, LCD_W, sscale(36, clkScale) + 2, LCD_BLACK);
        }
        drawClockSeconds(gfx, tbuf, lastTimeStr, CLOCK_BASELINE, currentTheme().clock, clkScale);
        strncpy(lastTimeStr, tbuf, sizeof(lastTimeStr) - 1);
        lastTimeStr[sizeof(lastTimeStr) - 1] = '\0';
    } else {
        gfx->fillRect(0, CLOCK_BASELINE - sscale(36, clkScale) - 2, LCD_W, sscale(36, clkScale) + 2, LCD_BLACK);
        static const char* t = "同步时间中...";
        uint16_t syncScale = scaleFromPx(configManager.clockFontSize, 34);
        drawUtf8(gfx, t, (LCD_W - utf8Width(t, syncScale)) / 2, CLOCK_BASELINE, currentTheme().sync, syncScale);
        lastTimeStr[0] = '\0';
    }

    // 农历行 (独立, CJK 大字)
    if (dateChanged && clock.valid) {
        char lunarbuf[16];
        LunarCalendar::text(clock.year, clock.mon, clock.day, lunarbuf, sizeof(lunarbuf));
        if (lunarbuf[0] != '\0') {
            uint16_t lScale = scaleFromPx(configManager.lunarFontSize, 16);
            int16_t lh = sscale(18, lScale) + 2;
            gfx->fillRect(0, LUNAR_BASELINE - lh, LCD_W, lh, LCD_BLACK);
            int16_t lw = utf8Width(lunarbuf, lScale);
            drawUtf8(gfx, lunarbuf, (LCD_W - lw) / 2, LUNAR_BASELINE, currentTheme().lunar, lScale);
        }
    }

    // 天气行: 区+天气+气温 (文本组居中), 右侧刷新倒计时饼图 (直径=行高)
    uint16_t wScale = scaleFromPx(configManager.weatherFontSize, 16);
    int16_t wh = sscale(20, wScale) + 2;
    const char* district = city;  // city 完整显示, 不再截取
    int16_t dw = utf8Width(district, wScale);
    int16_t ww = utf8Width(wlbuf, wScale);
    int16_t tw = dw + sscale(8, wScale) + ww;
    int16_t pieD = dateBarH;                 // 饼图直径 = 日期行高 (上下对齐)
    int16_t gap = 6;
    int16_t x0w = (LCD_W - (tw + gap + pieD)) / 2;
    if (weatherChanged) {
        gfx->fillRect(0, WEATHER_BASELINE - wh, LCD_W, wh, LCD_BLACK);
        drawUtf8(gfx, district, x0w, WEATHER_BASELINE, currentTheme().weatherArea, wScale);
        drawUtf8(gfx, wlbuf, x0w + dw + sscale(8, wScale), WEATHER_BASELINE, currentTheme().weatherInfo, wScale);
    }
    // 饼图: 剩余进度 (从满到空). 刚拉取完 frac≈1 (满圆), 随时间递减到 0.
    {
        unsigned long nowW = millis();
        unsigned long wNext = ClockWeather::nextWeatherMs();
        unsigned long wInt = (unsigned long)configManager.weather_min * 60000UL;
        float frac = 0.0f;
        if (wInt > 0) {
            long rem = (long)(wNext - nowW);   // 环形差; 未知/过期 -> <=0
            if (rem > 0) {
                frac = (float)rem / (float)wInt;
                if (frac > 1.0f) frac = 1.0f;
            }
        }
        int pct = (int)(frac * 1000.0f);
        if (pct != lastPiePct || weatherChanged) {
            lastPiePct = pct;
            int16_t pieCx = x0w + tw + gap + pieD / 2;
            int16_t pieCy = WEATHER_BASELINE - wh / 2;
            int16_t r = pieD / 2;
            gfx->fillRect(pieCx - r - 1, pieCy - r - 1, pieD + 2, pieD + 2, LCD_BLACK);
            drawPie(gfx, pieCx, pieCy, r, frac, currentTheme().pie);
        }
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
                uint16_t ns = scaleFromPx(configManager.serviceFontSize, 16);
                drawUtf8(gfx, t, (LCD_W - utf8Width(t, ns)) / 2, SVC_TOP + 20, currentTheme().svcNone, ns);
            } else {
                uint16_t svcScale = scaleFromPx(configManager.serviceFontSize, 8);
                int16_t adv = sscale(6, svcScale);
                int16_t addrW = adv * 16 + 40;   // 地址定宽: 最长16字符*adv + 40px
                int pageStart = svcPage * 2;
                int rowsOnPage = (n - pageStart >= 2) ? 2 : (n - pageStart);
                int16_t textH = sscale(8, svcScale);
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
                    drawGlcd(gfx, lbl, x + 10, y, currentTheme().svcAddr, svcScale);
                    if (svcs[i].up) {
                        char ms[16];
                        snprintf(ms, sizeof(ms), "%dms", svcs[i].latency_ms);
                        drawGlcd(gfx, ms, x + addrW + SVC_GAP, y, svcDelayColor(svcs[i].latency_ms), svcScale);
                    } else {
                        drawGlcd(gfx, "down", x + addrW + SVC_GAP, y, (uint16_t)0xF410, svcScale);
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