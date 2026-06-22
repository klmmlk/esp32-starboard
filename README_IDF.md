# ink_test → ESP-IDF 移植说明(方案 A:Arduino 作为组件)

不改任何显示逻辑。把 `arduino-esp32` 当作 ESP-IDF 的 component 引入,原 `ink_test.ino`
几乎原样保留,只套了一个 `app_main()` 外壳。代码见 `main/main.cpp`。

## 第 0 步:装 ESP-IDF 并确认版本

`arduino-esp32` 版本必须和 ESP-IDF 严格匹配,否则必然编译失败:

| ESP-IDF     | arduino-esp32 |
|-------------|---------------|
| v5.5.x      | 3.3.x         |
| v5.4.x      | 3.2.x         |
| v5.3.x      | 3.1.x         |
| v5.1.x      | 3.0.x         |

推荐装 **ESP-IDF v5.3.x 或 v5.4.x**(LTS,稳定)。查版本:`idf.py --version`

## 第 1 步:放三个依赖库(必须手动,它们不在 Component Registry)

| 库 | 地址 | 放到哪 |
|----|------|--------|
| Adafruit_BusIO | https://github.com/adafruit/Adafruit_BusIO | `components/Adafruit_BusIO/`(仓库根文件) |
| Adafruit_GFX | https://github.com/adafruit/Adafruit-GFX-Library | `components/Adafruit_GFX/`(根文件 + `Fonts/`) |
| GxEPD2 | https://github.com/ZinggJM/GxEPD2 | `components/GxEPD2/src/`(把它的 `src/` 内容拷进来) |

每个目录里已写好对应的 `CMakeLists.txt`,不用动。拷库时注意:
- **Adafruit_GFX 不要拷 `fontconvert/` 和 `examples/` 子目录**(带桌面 main,会冲突)。
- 三个库各自的 `CMakeLists.txt` 已经就位,别覆盖。

`arduino-esp32` 不用手动放,第 2 步自动拉取。

## 第 2 步:选 arduino-esp32 版本

打开 `main/idf_component.yml`,按第 0 步表把 `version` 改成和你 IDF 版本对应的那一行。

## 第 3 步:设置目标芯片

```
idf.py set-target esp32s3
```

## 第 4 步:菜单配置(按需)

```
idf.py menuconfig
```
- **PSRAM**:对应原 `arduino.json` 的 `PSRAM=enabled`。4.2 寸三色屏整屏缓冲约 15 KB×3 ≈ 需要较大内存,建议开:
  `Component config → ESP PSRAM → Support for external SPI-connected RAM`
- **Flash 大小**:对应原 8M。`Serial flasher config → Flash size → 8 MB`

## 第 5 步:编译、烧录、看串口

```
idf.py build
idf.py -p COM3 flash monitor      # Windows,COM 号换成你的;Mac/Linux 是 /dev/tty.usbmodem-*
```

按 `Ctrl+]` 退出 monitor。

## 常见坑速查

| 现象 | 原因 / 处理 |
|------|------------|
| 一堆 `undefined reference to ...` | arduino-esp32 和 IDF 版本不匹配,回第 0 步 |
| `Fonts/FreeMonoBold9pt7b.h: No such file` | Adafruit_GFX 没拷全,`Fonts/` 目录要带上 |
| `multiple definition of main` | Adafruit_GFX 误把 `fontconvert/` 拷进来了,删掉它 |
| 屏不亮 / 全黑 | 多半是驱动型号。确认是 `GxEPD2_420c_GDEY042Z98`(4.2 寸三色) |
| SPI 不通 | 引脚接的是默认 SPI(CS=10/SCK=12/MOSI=11),和 Arduino 下一致;如改过线,在构造函数第 5 个参数传自定义 `SPI` 对象 |
