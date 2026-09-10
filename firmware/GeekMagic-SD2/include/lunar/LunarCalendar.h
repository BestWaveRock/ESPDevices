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

#ifndef LUNAR_CALENDAR_H
#define LUNAR_CALENDAR_H

#include <cstdint>

namespace LunarCalendar {
    struct Result {
        int month;     // 1..12
        int day;       // 1..30
        bool leap;     // 闰月
        bool valid;    // 是否在表范围内
    };

    // 公历 -> 农历 (查表, 2020-2050)
    Result convert(int year, int month, int day);

    // 农历日 -> 中文 ("初一".."三十")
    const char* dayText(int day);

    // 农历月 -> 中文 ("正".."十二")
    const char* monthText(int month);

    // 完整: "七月廿九" 或 "闰七月廿九" (写入 buf)
    const char* text(int year, int month, int day, char* buf, int bufsize);
}  // namespace LunarCalendar

#endif  // LUNAR_CALENDAR_H
