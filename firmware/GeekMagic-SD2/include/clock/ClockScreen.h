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

#ifndef CLOCK_SCREEN_H
#define CLOCK_SCREEN_H

#include <Arduino.h>
#include "display/DisplayManager.h"  // Arduino_GFX + LCD_W/LCD_H + colors

namespace ClockScreen {
    // 用 CJK 字体绘制 UTF-8 字符串 (y 为基线), s2=半单位缩放(2=1x, 1=0.5x, 3=1.5x, 4=2x)
    void drawUtf8(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color, uint8_t s2 = 2);
    // 用时钟数字字体绘制 (y 为基线)
    void drawClock(Arduino_GFX* gfx, const char* s, int16_t x, int16_t baseline, uint16_t color);
    // 测量 CJK 字符串宽度 (s2=半单位缩放)
    int16_t utf8Width(const char* s, uint8_t s2 = 2);
    int16_t clockWidth(const char* s);
    // 渲染主屏幕
    void render(Arduino_GFX* gfx);
}  // namespace ClockScreen

#endif  // CLOCK_SCREEN_H
