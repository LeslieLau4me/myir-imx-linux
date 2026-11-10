# MYC-LMX9X 引脚改版（Pin List v2）设备树可行性评估报告

| 项目 | 内容 |
|---|---|
| 评估日期 | 2026-10-15 |
| 基线设备树 | `arch/arm64/boot/dts/myir/myd-lmx9x-11x11-custom.dts` |
| 对照对象 | `MYC-LMX9X Pin List-v1`（现版）→ `MYC-LMX9X Pin List-v2`（改版） |
| 引脚复用头文件 | `arch/arm64/boot/dts/myir/imx93-pinfunc.h` |
| 参考模板 | `arch/arm64/boot/dts/myir/myd-lmx9x-11x11.dts`（基板，含 RGMII / flexcan1） |
| 评估结论 | ✅ **可行** —— 全部改动可在 i.MX93 设备树环境内通过 pinctrl 与外设节点调整实现，无需改动内核驱动（GPIO 消费节点除外） |

---

## 1. 背景与目的

MYC-LMX9X 核心板（i.MX93，11x11，BGA306）进行硬件改版，引脚功能分配由 Pin List v1 变更为 v2。本次评估的目的：

1. 判定 v2 的引脚配置能否在现有 i.MX93 设备树环境中实现；
2. 输出完整的设备树改动清单（pinctrl 组、外设节点、使能/禁用）；
3. 识别需硬件确认的事项。

## 2. 结论摘要

1. **可行**：v2 涉及的所有引脚复用（千兆 RGMII 以太网、CAN1、各类 GPIO）在 `imx93-pinfunc.h` 中均有对应宏；基板 `myd-lmx9x-11x11.dts` 已提供 RGMII 与 flexcan1 的现成 pinctrl 模板，可直接复用。
2. **核心变化**：以太网由百兆 RMII 升级为千兆 RGMII，ENET1 的 4 个备用引脚（TD3/TXC/RD2/RD3）被收回用于以太网，原挂在上面的一批功能（确认键、两个 LED、WIFI 3.3V 检测）迁至其它 GPIO。
3. **无复用硬冲突**：各功能引脚互不冲突；被删除的控制器（lpuart6、lpi2c2）需在设备树中 `disabled`。
4. **硬件确认结论**：
   - ① 加密芯片所挂的 "I2C1_1V8"（B41/B42）为板上经电平转换的 I2C1 分支，归属 **lpi2c1**（硬件工程师已确认）；
   - ② 电压域（1.8V/3.3V）匹配已通过板上电平转换芯片解决。

---

## 3. 硬件改版变更分析（v1 → v2）

### 3.1 以太网：百兆 RMII → 千兆 RGMII（本次最大改动）

| 引脚 | BGA | v1 功能 | v2 功能 |
|---|---|---|---|
| ENET1_TD3 | V12 | 确认（掉电电源关闭）按键 IO | **千M以太网（RGMII_TD3）** |
| ENET1_TXC | U10 | L-RUN LED 控制 IO | **以太网（TX_CLK）** |
| ENET1_RD2 | AA9 | WIFI电源3.3V检测 | **以太网（RGMII_RD2）** |
| ENET1_RD3 | Y10 | L-ALARM LED 控制 IO | **以太网（RGMII_RD3）** |
| ENET1_TD0/TD1/TD2/TX_CTL/RD0/RD1/RX_CTL/RXC/MDC/MDIO | — | 以太网（RMII） | 千M以太网（RGMII） |

> 说明：RGMII 需要完整 4 组 TD/RD 数据线与 TXC/RXC 时钟线，故 ENET1 全部引脚被以太网占用，原挂在备用脚上的功能全部迁出。

### 3.2 被 ENET1 收回后迁移的功能

| 功能 | v1 位置 | v2 位置 |
|---|---|---|
| 确认（掉电电源关闭）按键 | ENET1_TD3 / GPIO4_02 | **SAI1_TXFS / GPIO1_11** |
| L-RUN LED | ENET1_TXC / GPIO4_07 | **GPIO2_IO04** |
| L-ALARM LED | ENET1_RD3 / GPIO4_13 | **GPIO2_IO05** |
| 开模块盖检测 | SAI1_TXFS / GPIO1_11 | **CLKO03 / GPIO4_28** |
| 4000&5000 硬件版本 IO 检测 | CLKO03 / GPIO4_28 | ❌ 已删除 |

### 3.3 串口用途变更

| 串口 | 引脚 | v1 | v2 |
|---|---|---|---|
| lpuart6 | GPIO2_IO04 / GPIO2_IO05 | PLC 串口 | ❌ 删除（引脚改做 LED） |
| lpuart3 | GPIO2_IO14 / GPIO2_IO15 | 红外串口 | **485-2 串口**（引脚不变，改用途） |

### 3.4 新增 CAN

| 引脚 | BGA | v1 功能 | v2 功能 |
|---|---|---|---|
| PDM_CLK | G17 | 4G模块电源控制 IO | **CAN（CAN1_TX）** |

> 说明：PDM_DATA0（原 4G USB 电源控制）在 v2 改为 GPIO「4G模块5V控制」，不再承担 CAN_RX（见 3.6）。

### 3.5 I2C 重排

| 总线 | v1 | v2 |
|---|---|---|
| I2C2（I2C2_SDA/SCL，GPIO1_03/02） | 加密芯片 I2C | **两个引脚改 GPIO**：GPIO1_03 = 4G模块电源控制、GPIO1_02 = 以太网模块5V电源控制 |
| I2C1_1V8（B41/B42） | 空 | **加密芯片 I2C** |
| I2C1 3.3V（C38/C39） | RTC I2C | RTC I2C（不变） |

### 3.6 其它 GPIO 改名 / 新增

| 引脚 | v1 功能 | v2 功能 |
|---|---|---|
| SAI1_TXD0 / GPIO1_13 | PLC模块复位 IO | RF模块电源开关控制 |
| SAI1_RXD0 / GPIO1_14 | 4G模块复位 IO | 485模块插拔检测 |
| GPIO2_IO17 | SIM卡切换 IO | RF模块插拔检测 |
| GPIO2_IO18 | 空 | 485模块电源控制 |
| PDM_DATA0 / GPIO1_09 | 4G USB 电源控制 IO | 4G模块5V控制 |

### 3.7 被删除的功能清单

| 功能 | 原引脚 |
|---|---|
| 4000&5000 硬件版本 IO 检测 | CLKO03 / GPIO4_28 |
| PLC 串口（lpuart6） | GPIO2_IO04 / GPIO2_IO05 |
| PLC 模块复位 IO | SAI1_TXD0 / GPIO1_13 |
| 4G 模块复位 IO | SAI1_RXD0 / GPIO1_14 |
| SIM 卡切换 IO | GPIO2_IO17 |
| WIFI 电源 3.3V 检测 | ENET1_RD2 / GPIO4_12 |
| 红外串口（lpuart3 原用途） | GPIO2_IO14 / GPIO2_IO15 |

### 3.8 USB（仅语义变更）

| 端口 | v1 | v2 |
|---|---|---|
| USB1 | U盘 | U盘、烧录 USB |
| USB2 | 4G模块 USB 通信口 | 各通信模块 USB HUB 输入 |

---

## 4. 设备树可行性核对

### 4.1 复用资源核对（均已确认存在）

| 需求 | 复用宏 | 核验结果 |
|---|---|---|
| 千兆 RGMII 数据/控制 | `MX93_PAD_ENET1_RD2/RD3/TD3__ENET_QOS_RGMII_*` | ✅ 存在 |
| RGMII 时钟 | `ENET1_RXC__CCM_ENET_QOS_CLOCK_GENERATE_RX_CLK`、`ENET1_TXC__CCM_ENET_QOS_CLOCK_GENERATE_TX_CLK` | ✅ 基板在用 |
| CAN1（TX） | `MX93_PAD_PDM_CLK__CAN1_TX`（mux 0x6） | ✅ 存在（基板 flexcan1 在用） |
| LED 迁移 | `MX93_PAD_GPIO_IO04/05__GPIO2_IO04/05`（mux 0） | ✅ 存在 |
| I2C2 → GPIO | `MX93_PAD_I2C2_SDA__GPIO1_IO03`、`MX93_PAD_I2C2_SCL__GPIO1_IO02`（mux 0x5） | ✅ 存在 |
| 其余 GPIO | SAI1_TXFS/TXD0/RXD0、CCM_CLKO3、GPIO2_IO17/18 等 | ✅ 存在 |

### 4.2 复用冲突检查

| 检查项 | 结果 |
|---|---|
| ENET2 全部分给 LCD/按键/开关量，`fec` 需保持 disabled | ✅ 无冲突 |
| SD3 分给 LCD 控制 + RELAY1，`usdhc3` 需保持 disabled | ✅ 无冲突 |
| SD2_CD_B 做 BT_EN，`usdhc2`(WiFi SDIO) 不占用 CD_B | ✅ 无冲突 |
| GPIO2_IO28/29 做继电器（复用含 i2c3），`lpi2c3` 需保持 disabled | ✅ 无冲突 |
| CLKO01 做以太网复位（GPIO3_26），与 eqos mdio reset-gpios 对应 | ✅ 无冲突 |
| lpuart6 删除后 GPIO2_IO04/05 改 LED，无其它占用 | ✅ 无冲突 |
| lpi2c2 删除后 GPIO1_02/03 改 GPIO，无其它占用 | ✅ 无冲突 |

---

## 5. 确认事项（硬件确认结论）

| 序号 | 事项 | 确认结论 | 对 DTS 的影响 |
|---|---|---|---|
| ① | 加密芯片所挂 "I2C1_1V8" 总线（B41/B42） | 为板上经电平转换的 I2C1 分支，归属 **lpi2c1** | 加密芯片挂 lpi2c1；lpi2c2 disabled，其引脚改 GPIO |
| ② | 电压域复核（ENET1/ENET2/SD3/CLK 为 1.8V 域） | 已加电平转换芯片解决 | 无，pad 电气配置值按原值即可 |

---

## 6. 建议的设备树改动方案

### 6.1 以太网（eqos：RMII → RGMII 千兆）

- `phy-mode`：`"rmii"` → `"rgmii-id"`
- `pinctrl_eqos` 换成完整 RGMII（参考基板 `myd-lmx9x-11x11.dts` 第 757-774 行）：
  `ENET1_MDC/MDIO`、`TD0/TD1/TD2/TD3`、`TX_CTL`、`TXC→CCM_ENET_QOS_CLOCK_GENERATE_TX_CLK`、`RD0/RD1/RD2/RD3`、`RX_CTL`、`RXC→CCM_ENET_QOS_CLOCK_GENERATE_RX_CLK`
- 移除 RMII 专用的 50MHz `assigned-clocks` 配置
- 保留 `pinctrl_reset_eqos`（CLKO01 / GPIO3_26 以太网复位，v2 仍标注）

### 6.2 CAN（flexcan1）

- `&flexcan1`：`status = "okay"` + `pinctrl-0 = <&pinctrl_flexcan1>`
- `pinctrl_flexcan1`：`MX93_PAD_PDM_CLK__CAN1_TX`（PDM_DATA0 已改 GPIO「4G模块5V控制」，不做 CAN_RX）

### 6.3 串口

- `&lpuart6`：`status = "disabled"`（PLC 串口删除）
- `&lpuart3`：保持 enabled，用途由「红外」改为「485-2」，rs485 配置按需调整

### 6.4 I2C

- `&lpi2c2`：`status = "disabled"`（加密芯片搬离，引脚改 GPIO）
- 加密芯片改挂 I2C1_1V8（板上电平转换的 I2C1 分支，归属 lpi2c1，已确认）

### 6.5 按键

- gpio-keys `key-enter`：`gpios = <&gpio4 2 ...>` → `<&gpio1 11 ...>`
- `pinctrl_gpio_keys`：`MX93_PAD_ENET1_TD3__GPIO4_IO02` → `MX93_PAD_SAI1_TXFS__GPIO1_IO11`

### 6.6 LED

- drv_leds `leds-gpios`：`gpio4 7 / gpio4 13` → `gpio2 4 / gpio2 5`
- `pinctrl_led`：`ENET1_TXC__GPIO4_IO07`、`ENET1_RD3__GPIO4_IO13` → `GPIO_IO04__GPIO2_IO04`、`GPIO_IO05__GPIO2_IO05`

### 6.7 pinctrl_all_gpio 重排

**删除**：
- `MX93_PAD_ENET1_RD2__GPIO4_IO12`（收回做 RGMII）
- `MX93_PAD_PDM_CLK__GPIO1_IO08`（改 CAN1_TX，移入 pinctrl_flexcan1）

**注释 / 功能更新**：
- `SAI1_TXFS__GPIO1_IO11`：开模块盖检测 → 确认按键（实际移入 pinctrl_gpio_keys）
- `SAI1_TXD0__GPIO1_IO13`：PLC模块复位 IO → RF模块电源开关控制
- `SAI1_RXD0__GPIO1_IO14`：4G模块复位 IO → 485模块插拔检测
- `CCM_CLKO3__GPIO4_IO28`：4000&5000硬件版本IO检测 → 开模块盖检测
- `GPIO_IO17__GPIO2_IO17`：SIM卡切换 IO → RF模块插拔检测
- `GPIO_IO18__GPIO2_IO18`：闲置 → 485模块电源控制
- `PDM_BIT_STREAM0__GPIO1_IO09`：4G USB 电源控制 → 4G模块5V控制

**新增**：
- `MX93_PAD_I2C2_SDA__GPIO1_IO03`：4G模块电源控制
- `MX93_PAD_I2C2_SCL__GPIO1_IO02`：以太网模块5V电源控制

---

## 7. 附录：v2 功能分配总表（按类别）

### 7.1 通信接口

| 类别 | 引脚 | v2 功能 | 设备树节点 |
|---|---|---|---|
| RS232 | UART1_TXD/RXD | RS232串口 | lpuart1 |
| 485 | UART2_TXD/RXD | 485串口 | lpuart2 |
| 485-2 | GPIO2_IO14/15 | 485-2串口 | lpuart3 |
| 协处理器 | GPIO2_IO00/01 | 协处理器串口 | lpuart5 |
| 蓝牙 | GPIO2_IO08/09 | 蓝牙串口 | lpuart7 |
| 计量 | GPIO2_IO12/13 | 计量串口 | lpuart8 |
| CAN | PDM_CLK | CAN1_TX | flexcan1 |
| 以太网 | ENET1 全部 | 千M以太网（RGMII） | eqos |
| RTC I2C | I2C1_SDA/SCL (3.3V) | RTC I2C | lpi2c1 |
| 加密芯片 I2C | I2C1_1V8 (B41/B42) | 加密芯片 I2C | lpi2c1（板上电平转换分支） |

### 7.2 按键

| 功能 | 引脚 | GPIO |
|---|---|---|
| 下按键 | ENET2_MDIO | GPIO4_15 |
| 取消按键 | ENET2_TD3 | GPIO4_16 |
| 上按键 | ENET2_RD3 | GPIO4_27 |
| 左按键 | ENET2_TXC | GPIO4_21 |
| 右按键 | GPIO2_IO06 | GPIO2_06 |
| 确认（掉电电源关闭）按键 | SAI1_TXFS | GPIO1_11 |

### 7.3 LCD / LED / 显示

| 功能 | 引脚 | GPIO |
|---|---|---|
| LCD-RES | SD3_DATA2 | GPIO3_24 |
| LCD-A0/WR/RD/CS | SD3_CLK/CMD/DATA0/DATA1 | GPIO3_20/21/22/23 |
| LCD-D0~D7 | ENET2_RD0/RD1/RXC/TX_CTL/TD2/TD1/TD0/RX_CTL | GPIO4_24/25/23/20/17/18/19/22 |
| LCD 液晶控制（背光） | PDM_DATA1 | GPIO1_10 |
| L-RUN LED | GPIO2_IO04 | GPIO2_04 |
| L-ALARM LED | GPIO2_IO05 | GPIO2_05 |

### 7.4 GPIO 控制 / 检测

| 功能 | 引脚 | GPIO |
|---|---|---|
| WIFI电源开关控制 | SAI1_TXC | GPIO1_12 |
| WIFI WLAN EN | GPIO2_IO27 | GPIO2_27 |
| BT_EN | SD2_nCD | GPIO3_00 |
| RF模块电源开关控制 | SAI1_TXD0 | GPIO1_13 |
| RF模块插拔检测 | GPIO2_IO17 | GPIO2_17 |
| 485模块插拔检测 | SAI1_RXD0 | GPIO1_14 |
| 485模块电源控制 | GPIO2_IO18 | GPIO2_18 |
| 4G模块电源控制 | I2C2_SDA | GPIO1_03 |
| 4G模块5V控制 | PDM_DATA0 | GPIO1_09 |
| 4G模块插拔检测 | GPIO2_IO19 | GPIO2_19 |
| 以太网模块5V电源控制 | I2C2_SCL | GPIO1_02 |
| 以太网复位 | CLKO01 | GPIO3_26 |
| 开模块盖检测 | CLKO03 | GPIO4_28 |
| 开尾盖检测 | GPIO2_IO02 | GPIO2_02 |
| 开盖检测 | GPIO2_IO22 | GPIO2_22 |
| 开关量输入1/2/3 | GPIO2_IO16/03、ENET2_RD2 | GPIO2_16/03、GPIO4_26 |
| 掉电检测 | GPIO2_IO21 | GPIO2_21 |
| PLC模块电源控制 | ENET2_MDC | GPIO4_14 |
| PLC模块插拔检测 | GPIO2_IO20 | GPIO2_20 |
| 外置电池供电控制 | GPIO2_IO07 | GPIO2_07 |
| 外置电池充电使能 | GPIO2_IO23 | GPIO2_23 |
| 锂电池电压检测控制 | CLKO02 | GPIO3_27 |
| 校表485切换 | GPIO2_IO26 | GPIO2_26 |
| 继电器ON/OFF | GPIO2_IO29/28 | GPIO2_29/28 |
| 非保持RELAY_1控制 | SD3_DATA3 | GPIO3_25 |

### 7.5 PWM / ADC

| 功能 | 引脚 | 说明 |
|---|---|---|
| 远红外38KHz输出 | GPIO2_IO24 | TPM3_CH3 |
| 蜂鸣器控制 | GPIO2_IO25 | TPM4_CH3 |
| 外部电池电压检测 | ADC_IN0 | adc1 |
| 锂电池电压检测 | ADC_IN1 | adc1 |

---

*报告完。*


