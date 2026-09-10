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

#include "lunar/LunarCalendar.h"
#include "lunar/lunar_table.h"
#include <string.h>
#include <stdio.h>
#include <pgmspace.h>

namespace LunarCalendar {

static bool isLeap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const uint16_t DAYS_BEFORE[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

int dayOfYear(int y, int m, int d) {
    return DAYS_BEFORE[m - 1] + d + (isLeap(y) && m > 2 ? 1 : 0);
}

Result convert(int year, int month, int day) {
    Result r{0, 0, false, false};
    if (year < LUNAR_BASE_YEAR || year > LUNAR_BASE_YEAR + 30) return r;
    int doy = dayOfYear(year, month, day);
    int idx = (year - LUNAR_BASE_YEAR) * LUNAR_STRIDE + (doy - 1);
    uint16_t e = pgm_read_word((const uint16_t*)&LunarTable[idx]);
    if (e == 0) return r;
    r.day = e & 0x1F;
    r.month = (e >> 5) & 0x0F;
    r.leap = (e & 0x200) != 0;
    r.valid = true;
    return r;
}

const char* dayText(int day) {
    static const char* D[] = {
        "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
    if (day < 1 || day > 30) return "";
    return D[day - 1];
}

const char* monthText(int month) {
    static const char* M[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
    if (month < 1 || month > 12) return "";
    return M[month - 1];
}

const char* text(int year, int month, int day, char* buf, int bufsize) {
    if (bufsize <= 0) return "";
    buf[0] = '\0';
    Result r = convert(year, month, day);
    if (!r.valid) return buf;
    int off = 0;
    if (r.leap) off += snprintf(buf + off, bufsize - off, "闰");
    off += snprintf(buf + off, bufsize - off, "%s", monthText(r.month));
    snprintf(buf + off, bufsize - off, "%s", dayText(r.day));
    return buf;
}

}  // namespace LunarCalendar
