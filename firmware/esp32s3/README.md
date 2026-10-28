# ESP32-S3 + FSR402 + 3V ERM 测试（PlatformIO）

仅体验 ERM 振动的最小程序见 [SIMPLE_ERM.md](SIMPLE_ERM.md)，使用 `simple_erm` 配置。
用户提供的已工作 Adafruit 程序对照测试见 [ADAFRUIT_ERM.md](ADAFRUIT_ERM.md)，使用 `adafruit_erm`。
完整的 FSR → threshold → 实测 gain reduction → ERM 测试见 [GR_ERM.md](GR_ERM.md)，使用 `mixsense_gr_erm`。

用户提供的商品标题是“5PCS 10x2mm Mini DC Vibration Motors 3V 12000rpm Flat(2.7mm) …”。
据此按 **3V 旋转式 ERM 电机**测试，不采用 LRA 模式。原商品页尚无法读取，
3V/12000rpm 来自用户提供的标题；未把转售页的电流/过驱参数作为已确认规格。
[用户商品链接](https://www.amazon.ca/dp/B0BVJR8LTN)。12000rpm 是转速，不是 LRA 谐振频率。

## 已确认接线

| 连接 | 引脚/电源 |
|---|---|
| FSR402 + 2.2kΩ/10kΩ 电阻的测量节点 | GPIO5 |
| DRV2605L SDA | GPIO9 |
| DRV2605L SCL | GPIO8 |
| DRV2605L 供电 | 3.3V，与 ESP32 共地 |
| ERM 红线 / 黑线 | DRV2605L OUT+ / OUT− |
| 电脑连接 | 开发板原生 USB port |

电机接差分输出 OUT+/OUT−，两线都不接 GPIO，黑线也不是接 ESP32 GND。
IN/TRIG 不接 ESP32；TI 要求不用时接地，若 breakout 没有板载下拉则接 GND。
确认 I²C 上拉接到 3.3V。用户当前 breakout 没有外露 EN；不要将 IN 当作 EN 接 3.3V。
芯片 EN 的处理依照该板原理图；TI 说明 EN 为低时仍可 ACK，因此不能将地址 NACK 直接归因于 EN。

当前用户报告的 FSR 接法：`3.3V → 新增 2.2kΩ → FSR402 → 测量节点 → 原有 10kΩ → GND`，
节点接 GPIO5。两个固定电阻都保留，新增电阻串联在 FSR 的供电侧。
这使理想最大节点电压限制在约 `3.3 × 10/(2.2+10) = 2.70V`。
通常随压力增加读数增加；不要用 5V 给该分压供电。映射表示相对压力，不是牛顿。
2026-10-25 初次实测采用只有一个 10kΩ 的分压，重压达到 4095；
用户随后报告加入供电侧 10kΩ，但复测仍达到 4095，且驱动 STATUS 读取失败。
随后用户报告改为 2.2kΩ/10kΩ；该次捕获没有达到 ADC 上限，但报告松开后读数仍约 3350，
且驱动 STATUS 读取仍失败。当前需要核对实际接线和释放状态，尚未据此校准。

## 配置选择

用 VS Code / PlatformIO 打开本目录，保留完整仓库以复用共享协议编码器。不需要 JUCE。

| 配置 | 用途 | 会驱动电机 |
|---|---|---|
| `fsr_monitor`（默认） | USB 文本显示 ADC | 否 |
| `hardware_check` | ADC + I²C 0x5A 芯片识别 | 否 |
| `mixsense_usb` | FSR → MixSense；解析反馈 | 否 |
| `erm_test` | USB 文本命令，500ms 振动测试 | 仅收到按键后 |
| `mixsense_erm` | MixSense 双向协议 + ERM 输出 | 仅收到有效 haptic 命令后 |
| `mixsense_gr_erm` | FSR 反向映射 threshold + 双向协议 + Adafruit ERM，RTP 上限 90 | 仅收到有效 haptic 命令后 |

原生 USB 使用 OTG CDC（USB_MODE=0），GPIO19/20 留给 USB。
采用 DevKitC-1 通用 S3 配置。已通过 esptool 确认当前实物为 ESP32-S3 revision v0.2、
16MB quad-SPI Flash；分区仍使用前 4MB，不启用 PSRAM（PSRAM 变体尚未确认）。
首次烧录失败：按住 BOOT，点 RESET，松开 BOOT，
重新选端口。烧录后 `/dev/cu.*` 名字可能变化。

下列命令从**仓库根目录**运行；若在本项目目录运行，省略 `-d firmware/esp32s3`。
使用实际端口替换 `/dev/cu.YOUR_PORT`。

## 1. 先确认 FSR 与 I²C

断电后先断开电机，保留电源、共地、SDA、SCL，再上传：

```sh
pio device list
pio run -d firmware/esp32s3 -e hardware_check -t upload --upload-port /dev/cu.YOUR_PORT
pio device monitor --port /dev/cu.YOUR_PORT --baud 115200
```

预期 ADC 随按压变化，每秒出现 `device_id=7 DRV2605L`。
STATUS 其他位可能不同，不要求完整字节固定为 0xE0。读取失败时检查电源、共地、
SDA=9/SCL=8、EN 和上拉。识别成功不代表电机正常；此文本固件不连接 MixSense。

记录不按压与目标最大按压力度的 ADC，填入 `src/main.cpp` 的
`releasedAdc` / `pressedAdc`。0/3000 是占位值。默认 5ms 采样、指数平滑、
2% 归一化变化死区，变化包最多约 50 次/秒。

## 2. 独立 ERM 测试

关闭 Monitor，断电，将电机接到 OUT+/OUT−，再上电并上传：

```sh
pio run -d firmware/esp32s3 -e erm_test -t upload --upload-port /dev/cu.YOUR_PORT
pio device monitor --port /dev/cu.YOUR_PORT --baud 115200
```

看到 `ERM driver=READY` 后发送字符（可带回车）：

- `1`：50% 强度，500ms。
- `2`：75% 强度，500ms。
- `3`：100% 强度，500ms。
- `0`：清除测试脉冲及基线，电驱动归零。

上电不会自动振动。先试 `2`，没起转再试 `3`；低强度不足以克服起动摩擦不一定是故障。
FAULT 时先修正接线/供电再复位，不会自动恢复旧输出。不要把此文本固件连接 MixSense。

## 3. 与 MixSense 联调

关闭 Monitor，上传实际驱动电机的二进制协议固件：

```sh
pio run -d firmware/esp32s3 -e mixsense_erm -t upload --upload-port /dev/cu.YOUR_PORT
```

1. MixSense Standalone 填入原生 USB 的 `/dev/cu.*`，点 Connect CDC。
2. 松开 FSR 等待 500ms。固件发送 `0x06 CONTROL_SNAPSHOT`，UI 不应跳变。
3. 压力变化发送 `0x01 CONTROL_VALUE`（ID=0），更新 threshold。
   低值对应 -60dB，高值对应 0dB；重连重新快照并清除上一连接的输出。
4. UI 设置 refresh=100ms、source age=500ms（测试值），intensity 先试 0.75，
   点 Start / update persistent 应持续振动。低强度可能不能起转；
   全强度也只对应本固件设定的低压测试满幅。
5. Stop / request zero 将持续基线归零。
6. 保持 ESP32 供电，停止有效 haptic 命令，约 1 秒后电驱动应归零。
   原生 USB/端口状态变化也会清除基线及脉冲；重连需要重新 Start。
   电机有机械惯性，电驱动归零不等于转子瞬间停止。

FSR → threshold 与 UI → ERM 是独立链路，尚未实现压力/音频到振动的产品映射。
协议端口不打印文字、不增加自定义 ACK。文字日志用独立 UART 接口查看 Serial0；
不要在 MixSense 占用的原生 USB 端口打开 Monitor。connected 不代替实物接收/振动验证。

## 驱动策略与限制

- ERM **开环 RTP**：N_ERM_LRA=0 / ERM_OPEN_LOOP=1，signed RTP 正半轴 0..127。
  0 是零输出，不会把停止解释成反转。
- OD_CLAMP=0x70，按 TI Rev D 公式 6：`V_ERM_OL_AV = 0.02159 × 112 ≈ 2.418 V`。
  这是标称平均满幅电压，不是峰值硬上限或测量结果。供电补偿开启；
  不使用自动校准、ROM 或启动过驱。尚未做实物电压/温升/加速度校准。
- 连续 level 独立保存。pulse 最多 500ms，更新基线（含零）不截断当前脉冲；
  结束后返回最新基线。多个 pulse 采用新脉冲抢占旧脉冲的台架策略。
  时长是电驱动请求时间，不是实测可感振动时间。
- 仅有效 endpoint=0 haptic 命令刷新 1000ms 看门狗；visual、错误包不保活。
  超时/USB 状态变化取消脉冲并归零。
- 检查 I²C 结果、配置读回和驱动状态。故障锁定后只重试零输出/standby，
  修复后需复位，不自动重放强度。
- 停止保护依赖 MCU 循环与 I²C。总线断线但驱动板仍有电时停止写入可能失败，
  此时断开驱动板电源。该接线没有 MCU 控制的 EN，不保证总线失效时硬件关断。
- USB v1 无 checksum/ACK 的限制见 `docs/usb-protocol-v1.md`。

## 硬件外测试

仓库根目录运行：

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Iprotocol -Iplugin/Source/controller -Ifirmware/esp32s3/include \
  firmware/esp32s3/host_tests/ErmBenchTests.cpp \
  plugin/Source/controller/UsbFrameCodec.cpp -o /tmp/mixsense-erm-tests
/tmp/mixsense-erm-tests
```

覆盖脉冲/基线分离、抢占/限时、无效流量不保活、计时回绕、零输出编码、故障锁定、
读回失败、恢复不重放和分片协议包。模拟测试不证明物理电压或电机行为。

资料：
- [TI 数据手册](https://www.ti.com/lit/ds/symlink/drv2605l.pdf)：公式 6、8.5.4.1、图 26。
- [PlatformIO 开发板配置](https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html)
- [Espressif USB CDC](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html)
- [Espressif ADC](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html)
