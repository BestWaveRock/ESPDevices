# SD2 小电视 硬件说明

> 本文档基于官方开源仓库 [wfm123456/SD2AIO](https://gitee.com/wfm123456/SD2AIO) 的电路图、BOM 与可运行固件反推整理。
> 作者：王福敏（WFM），星光工作室。

## 一、设备概述

SD2 小电视是一款基于 ESP8266 的方形 1.54" 圆角 TFT 桌面小屏，出厂默认运行"星光工作室"商业固件
（天气/日历/股票/电子相册/闹钟，部分功能需激活码）。本仓库将其刷入
**GeekMagic Open Firmware**（开源、带 Web 面板、可 OTA）。

| 项目 | 规格 |
|------|------|
| 主控 | ESP8266EX（模组 ESP-12F，1MB Flash / 520KB RAM） |
| 屏幕 | ST7789 1.54" 方形 TFT，240×240，SPI 接口，RGB565 |
| 屏幕驱动 | 板上 SPI，无独立屏控 IC（ST7789 在屏内） |
| 背光 | PWM 调光，10bit |
| 串口 | CH340C（USB 转串口，带数据脚，可刷机/救砖） |
| 供电 | USB-C 5V，板载 AMS1117-3.3 |
| 可选触摸 | TTP223-H6（完整电路图有，**本实物 BOM 不含，未贴片**） |
| 可选音频 | CH7800 DAC + 喇叭（完整电路图有，**本实物 BOM 不含，未贴片**） |
| 可选温度 | DHT11（默认关闭） |

## 二、屏幕引脚映射（实测/固件反推，权威）

> 来源：官方固件 `TFT_eSPI/User_Setup.h`（PIN_Dx 为 NodeMCU 命名）+ `SmallDesktopDisplay.ino`，
> 与电路图 `SD2闹钟款带触摸感应按钮电路图.pdf` 交叉核对。**GeekMagic 固件按此刷入后可正常点亮。**

| 信号 | GPIO | NodeMCU | 说明 |
|------|------|---------|------|
| SCLK | 14 | D5 | SPI 时钟 |
| MOSI | 13 | D7 | SPI 数据 |
| CS   | 15 | D8 | 片选（**主动驱动**，非 GND） |
| DC   | 0  | D3 | 数据/命令（RS） |
| BL   | 5  | D1 | 背光 PWM |
| RST  | —  | —  | 无硬件复位脚，固件用软件复位/固定电平 |
| SPI 模式 | SPI_MODE3 | — | 40MHz |

> 关键点：CS 在 **GPIO15** 且由固件主动控制。GeekMagic 原仓库默认 `CS=-1`（GND 常低，
> 适配 HelloCubic 屏），必须改成 15 才能在本板点亮 —— 本仓库已改好。

## 三、完整电路图的其它功能脚（本实物未贴片，仅供参考）

| 功能 | 器件 | GPIO | 本实物 |
|------|------|------|--------|
| 触摸 | TTP223-H6 | 4 | 否（BOM 无 TTP223） |
| 音频 SPK1 | CH7800 | 12 | 否（BOM 无 CH7800） |
| 音频 SPK2 | CH7800 | 13 | 否 |
| 温度 | DHT11 | 12 | 否（默认关闭） |

> 固件中触摸/音频相关代码即使编译进去，因无硬件也不会生效，不影响使用。

## 四、Flash 分区布局（ESP8266 4MB / 2MB app）

```
0x000000  ── 0x1FFFFF   Application (firmware.bin, 含 eboot 引导)
0x200000  ── 0x3FFFFF   LittleFS 文件系统 (littlefs.bin, 网页 + config.json)
```

- `firmware.bin` 刷 `0x0`
- `littlefs.bin` 刷 `0x200000`（Web UI、GIF、config.json 都在里面）
- 只刷 firmware 不刷 littlefs 会导致 Web 页面 404 / Not Found。

## 五、配置文件

`data/config.json`（打包进 LittleFS，位于文件系统根目录 `/config.json`）：

```json
{
  "wifi_ssid": "",
  "wifi_password": "",
  "api_token": "geekmagic2026",
  "lcd_rotation": 4
}
```

- `lcd_rotation`：0~7，对应 ST7789 旋转；本屏默认 **4**（配合镜像修正）。
- 首次启动若 config 中的 WiFi 连不上，固件会回退到编译期内置的默认 WiFi
  （见 `src/main.cpp` 的 `DEFAULT_WIFI_SSID/PASSWORD`），并自动保存。

## 六、显示镜像修正

ST7789 的 MADCTL（0x36）寄存器含 MX（水平翻转，bit6=0x40）。GeekMagic 的 `setRotation()`
会按旋转值重写 MADCTL，本屏在 rotation=4 时内容左右镜像。
本仓库在 `src/display/DisplayManager.cpp` 增加 `lcdFlipHorizontalMirror()`，
在 `setRotation()` 之后再把 MX 位翻转一次，使内容方向正确（对所有旋转值均生效）。

## 七、救砖 / 串口

- USB-C 口连 Mac/PC，CH340 识别为串口（如 `/dev/cu.usbserial-*`）。
- 刷机波特率建议 **115200**（921600 在 ESP8266 上易报 `Invalid head of packet`）。
- 卡 boot loop 时可擦除 NVS：`esptool --chip esp8266 erase_region 0x9000 0x5000`。
