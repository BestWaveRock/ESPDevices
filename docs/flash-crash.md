# ESP8266 Flash 数据访问崩溃：问题、漏洞与修复过程

本文沉淀 SD2（ESP8266 + ST7789）上「把大量只读数据放进 flash」时踩到的全部坑，
以及最终稳定方案。涉及农历表（~23KB）、CJK/时钟字体位图（~4KB）等数据数组。

## 症状

- 设备反复 `Exception (3)` 崩溃重启，`rst cause:2`（复位）。
- 崩溃点 `excvaddr` 落在 `0x4025fxxx`/`0x40260xxx` 的 **数据区**，而非代码区。
- 堆内存极低（free heap 仅 ~300B），随后 OOM abort。

## 根因（逐层剥开）

### 1. `PROGMEM` 在本工具链里是「按变量命名的自定义 section」

本 PlatformIO 的 `pgmspace.h` 把 `PROGMEM` 定义为：

```c
#define PROGMEM __attribute__((section(".irom.text." FILE "." LINE "." COUNTER)))
```

即每个带 `PROGMEM` 的数组都落在一个 **以文件名/行号命名的私有 section**。
这些 section 链接后会与 `.irom0.text`（代码 + 字面量池）混在一起。

后果：
- 数据被塞进代码区，**挤占了字面量池（literal pool）** → 跳转指令的立即数错位
  → PC 跳到数据区 → 解释为指令 → `Exception 3`。
- 运行时按下标访问 `PROGMEM` 数组（`arr[i]`）会直接跳到数据区崩溃。

### 2. 去掉 `PROGMEM` 用裸 `const`：数据落进 RAM，OOM

去掉 `PROGMEM` 后，数据落进 `.rodata`（`0x3ffe8xxx`，即 RAM）。
23KB 农历表 + 4KB 字体直接吃掉 RAM，free heap 掉到 ~300B → OOM 崩溃。

### 3. 正确做法：`section(".irom.text")` + `pgm_read_*` 访问

关键两点：

**（a）数据放 `.irom.text`**（不是 `.irom0.text`）。链接脚本
`eagle.app.v6.common.ld.h` 中，`.irom0.text` 与 `.irom.text` 在**同一个输出段**，
`.irom.text` 会排在所有代码与字面量池**之后**，不会干扰代码/字面量池布局。

定义统一宏：

```c
#ifndef FLASHDATA
#define FLASHDATA __attribute__((section(".irom.text")))
#endif
static const uint16_t LunarTable[] FLASHDATA = { ... };
```

**（b）必须用 `pgm_read_byte/word/dword` 读取，不能用 `arr[i]` 直接访问。**

ESP8266（LX106）**不能对 flash 做非对齐的 8/16 位读取**（`l8ui`/`l16ui` 会触发
`Exception 3`）。`pgm_read_*` 宏展开后生成安全的 `l32i.n`（按字对齐加载）+
`ssa8l`/`srl` 移位序列，可安全读取 flash 任意字节。

### 4. 隐藏陷阱：编译器会把 `pgm_read` 优化成裸 load

即使代码里写了 `pgm_read_byte`，若**函数返回一个结构体**，编译器为传递返回值
可能直接对结构体字段发出裸 `l16ui`（如 `off` 字段），绕过 `pgm_read` → 又崩。

解决：
- `glyphAt` 改为 **输出参数**形式，不返回 `Glyph` 结构体：

```cpp
static void glyphAt(const Glyph* arr, uint16_t i, Glyph* out) {
    const uint8_t* b = (const uint8_t*)arr + (unsigned)i * sizeof(Glyph);
    out->off  = (uint16_t)((pgm_read_byte(b + 0) << 8) | pgm_read_byte(b + 1));
    out->w    = pgm_read_byte(b + 2);
    out->h    = pgm_read_byte(b + 3);
    out->xoff = (int8_t)pgm_read_byte(b + 4);
    out->yoff = (int8_t)pgm_read_byte(b + 5);
    out->adv  = pgm_read_byte(b + 6);
}
```

- `off` 用 **两个 `pgm_read_byte` 拼成 `uint16_t`**，而非一次 `pgm_read_word`，
  彻底杜绝裸 16 位 load。
- 用 `objdump` 反汇编 `glyphAt`，确认**只有 `l32i.n`/`s8i`/`s16i`/`ssa8l`/`srl`**，
  无任何 `l16ui`/`l8ui`，方可烧录。

## 最终方案清单

| 项 | 方案 |
|----|------|
| 数据 section | `FLASHDATA = __attribute__((section(".irom.text")))`（lunar_table.h、cjk_font.h） |
| 访问方式 | 一律 `pgm_read_byte/word/dword`，禁用裸 `arr[i]` |
| 结构体读取 | 输出参数形式 + 逐字节 `pgm_read_byte` 拼装，不返回结构体 |
| 位图渲染 | 直接传 flash 位图指针给 `drawBitmap`（其内部已用 `pgm_read_byte`，安全） |
| 验证 | `objdump -d` 确认无 `l16ui`/`l8ui`；烧录后串口无 `Exception` |

## 涉及文件

- `src/lunar/lunar_table.h` — `FLASHDATA` 宏 + `LunarTable`
- `src/lunar/LunarCalendar.cpp` — `pgm_read_word(&LunarTable[idx])`
- `src/fonts/cjk_font.h` — `FLASHDATA` + 位图/码点/度量数组
- `src/clock/ClockScreen.cpp` — `glyphAt`（输出参数 + 逐字节读取）

## 性能/资源结果

- Flash：45.8%（478759 / 1044464）
- RAM：54.4%（44552 / 81920），运行期 free heap 稳定 ~26KB
- 数据符号落在 `0x4025fxxx`/`0x40260xxx`（flash），运行无 `Exception`

## 字体乱码：字节序坑（只有 `0` 正常）

`Glyph.off` 是 `uint16_t`，从 flash 逐字节读。最初写成 **大端**：

```c
out->off = (pgm_read_byte(b+0) << 8) | pgm_read_byte(b+1);  // 错
```

但 ESP8266（LX106）是 **小端**：`b[0]` 是低字节、`b[1]` 是高字节。
结果所有字形的 `off` 偏移被交换——只有 `off=0`（两字节都是 0，`'0'`）正确，
其余字符位图指针全错 → 屏幕乱码。

修正为小端：

```c
out->off = (uint16_t)(pgm_read_byte(b+0) | (pgm_read_byte(b+1) << 8));  // 对
```

## 闪屏：每秒整屏重绘

`render()` 首句 `fillScreen(BLACK)`，而 DisplayManager 每秒调用一次 → 每秒整屏黑闪。

改为 **增量刷新**：记录上一帧的 时钟/日期/天气/服务 状态，仅当某分区内容变化时才
重绘该分区（重绘前只清该分区背景色）。首帧铺满整屏，之后只刷变化区 → 无闪屏。

## 天气/服务拉不到：三个叠加 bug

1. **`ClockWeather::update()` 没在 main loop 调用**——只有 `begin()`，天气/服务永不更新。
   修：`loop()` 里加 `ClockWeather::update();`。
2. **首拉条件永不满足**：`s_weatherTs=0` 且 `millis()` 从 0 起算，
   `now - s_weatherTs >= wInt(15min)` 要干等 15 分钟才第一次拉。
   修：改为「下次允许时间」`s_weatherNext=0`（0=立即），`now >= s_next` 触发后 `s_next = now + 间隔`。
3. **chunked HTTP 响应带前缀**：open-meteo 走 `Transfer-Encoding: chunked`，
   ESP8266HTTPClient 的 `getStream()` **不自动去 chunk**，body 前面多了一行 chunk-size：
   `31 37 66 0D 0A 7B …` = `17f\r\n{`。ArduinoJson 因此返回 `InvalidInput`。
   修：从 `body.indexOf('{')` 起解析。

> 诊断方法：把 body 前 16 字节做 hex dump 打印（`31 37 66 0D 0A 7B` 一眼看出 chunk 行），
> 这是定位「JSON 明明合法却解析失败」的关键。

## 教训

1. ESP8266 上 **`PROGMEM` 不可信**（按变量命名 section，破坏代码/字面量池布局）。
2. flash 数据用 `.irom.text` section，**不要**用 `.irom0.text`。
3. flash 数据 **必须** `pgm_read_*` 读，禁止裸下标；LX106 不支持 flash 非对齐 8/16 位读。
4. 避免函数返回含 `uint16`/`uint32` 字段的结构体，防止编译器优化出裸 load；用输出参数。
5. 烧录前用 `objdump` 校验关键函数无 `l16ui`/`l8ui`。
6. 逐字节拼 `uint16_t` 时 **注意小端**：`b[0]` 低字节、`b[1]` 高字节。
7. 周期性刷新 UI 用 **增量重绘**（只刷变化区），不要每秒 `fillScreen` 整屏。
8. 开源前 **移除硬编码的个人 WiFi 凭据**（本次已从 `main.cpp` 移除，改为空占位 + 为空时跳过内置连接）。
9. 周期任务用「下次允许时间」（`next=0` 表立即）而非「上次时间」，避免 `now-0` 首拉延迟。
10. ESP8266 的 `HTTPClient::getStream()` 对 chunked 响应**不去 chunk**，解析 JSON 前先从 `{` 开始。
11. 服务探测阻塞式 connect 会卡看门狗（2s），每次探测后 `yield()`。
