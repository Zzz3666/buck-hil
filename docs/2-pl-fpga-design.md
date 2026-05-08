# 2. PL (FPGA) 端模块设计

## 2.1 模块总览

```
┌────────────────────────────────────────────────────────────────┐
│                        PL 顶层                                  │
│  ┌───────────┐  ┌───────────┐  ┌──────────┐  ┌─────────────┐  │
│  │ pwm_cap   │  │buck_solver│  │dac_if    │  │capture_mgr  │  │
│  │           ├─►│           ├─►│          │  │             │  │
│  │ 400MHz    │  │ 100MHz    │  │ 50MHz    │  │ 100MHz      │  │
│  └───────────┘  └─────┬─────┘  └──────────┘  └──────┬──────┘  │
│                       │                              │        │
│                 ┌─────┴─────┐                   ┌────┴─────┐  │
│                 │ param_regs│                   │axi_s2mm  │  │
│                 │ AXI-Lite  │                   │ DMA 引擎  │  │
│                 └─────┬─────┘                   └────┬─────┘  │
│                       │                              │        │
│                 AXI Interconnect              AXI-Stream     │
└───────────────────────┼──────────────────────────┼───────────┘
                        │                          │
                        ▼                          ▼
                PS AXI GP (M_AXI_GP0)      PS AXI HP (S_AXI_HP0)
```

## 2.2 PWM 捕获模块 (`pwm_capture`)

### 2.2.1 功能需求

- 捕获被测控制器输出的 200kHz PWM 信号
- 输出占空比 Q16.16 定点数 (0x00000000 ~ 0x00010000)
- 抗毛刺：5ns 数字滤波
- 抗抖动：连续 3 周期占空比变化 < 1% 才更新输出
- 输出实测频率（诊断用）

### 2.2.2 接口定义

```verilog
module pwm_capture #(
    parameter CLK_FREQ        = 400_000_000,         // 计数器时钟
    parameter DEBOUNCE_TICKS  = 2,                    // 5ns @ 400MHz = 2 ticks
    parameter CONSISTENCY_CYCLES = 3                  // 一致性检查周期数
) (
    // 时钟和复位
    input  wire        clk,          // 400 MHz
    input  wire        rst_n,        // 异步复位，低有效

    // 外部信号
    input  wire        pwm_in,       // 来自控制器（经 3.3V 钳位）

    // 参数（来自 AXI 寄存器）
    input  wire [15:0] expected_freq, // 预期频率 (Hz)，用于异常检测

    // 输出给求解器
    output reg  [31:0] duty_q16,     // 占空比 Q16.16, 范围 [0.0, 1.0]
    output reg         duty_valid,   // 单周期脉冲：占空比已更新且通过一致性检查
    output reg  [15:0] measured_freq, // 实测频率 (Hz)
    output reg         freq_valid,   // 单周期脉冲：频率已更新
    output reg         pwm_lost       // PWM 信号丢失告警 (持续高电平)
);
```

### 2.2.3 状态机

```
          ┌───────┐
    ─────►│ IDLE  │◄────────────── 超时 ──────────────┐
          └───┬───┘                                    │
              │ 检测到上升沿                             │
              ▼                                        │
    ┌─────────────────┐                               │
    │ MEASURE_PERIOD   │                               │
    │ 计数直到下一个    │                               │
    │ 上升沿           │                               │
    └────────┬────────┘                               │
             │ 捕获完成                                │
             ▼                                        │
    ┌─────────────────┐                               │
    │ VALIDATE         │                               │
    │ 频率检查         │──── 异常 ────► [PWM_LOST] ────┘
    │ 毛刺检查         │
    └────────┬────────┘
             │ 通过
             ▼
    ┌─────────────────┐
    │ CONSISTENCY      │
    │ 连续N个周期      │
    │ 占空比差 < 1%?   │
    └────────┬────────┘
             │ 是 → 更新 duty_q16, duty_valid=1
             │ 否 → 丢弃, 回到 IDLE
             ▼
         [更新输出]
```

### 2.2.4 毛刺滤波

```verilog
// 数字低通滤波: 信号必须稳定 DEBOUNCE_TICKS 个周期才被认为是有效边沿
reg [DEBOUNCE_TICKS-1:0] pwm_sync_shift;
reg                      pwm_filtered;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        pwm_sync_shift <= 0;
        pwm_filtered   <= 0;
    end else begin
        // 两级同步器消除亚稳态
        pwm_sync_shift <= {pwm_sync_shift[DEBOUNCE_TICKS-2:0], pwm_in};

        // 多数表决：全 1 → 高, 全 0 → 低, 否则保持
        if (&pwm_sync_shift)
            pwm_filtered <= 1'b1;
        else if (|pwm_sync_shift == 1'b0)
            pwm_filtered <= 1'b0;
    end
end
```

### 2.2.5 跨域输出 (clk_pl_400 → clk_pl_100)

```verilog
// 在 400MHz 域生成单周期 pulse
// 在 100MHz 域用同步链 + 脉冲展宽捕获

// 400MHz 域:
reg duty_update_400;
duty_update_400 <= (state == UPDATE);

// 100MHz 域:
reg [2:0] duty_sync_ff;   // 3级同步链
reg       duty_update_stretched;
always @(posedge clk_100) begin
    duty_sync_ff <= {duty_sync_ff[1:0], duty_update_400};
    // 边沿检测 + 展宽（确保 100MHz 域能捕获 400MHz 的单周期脉冲）
    if (duty_sync_ff[2] ^ duty_sync_ff[1])
        duty_update_stretched <= 1'b1;
    else
        duty_update_stretched <= 1'b0;
end
```

---

## 2.3 Buck 求解器 (`buck_solver`)

### 2.3.1 数学模型

**开关状态模型 (Switched-State Model)**：

**状态1 — SW=ON (0 < t < D·Ts)**:
```
di_L/dt = (Vin - Vout - i_L · R_L) / L
dv_C/dt = (i_L - v_C / R_load) / C
```

**状态2 — SW=OFF (D·Ts < t < Ts)**:
```
di_L/dt = (-Vf - Vout - i_L · R_L) / L
dv_C/dt = (i_L - v_C / R_load) / C
```

### 2.3.2 离散化

**方法**：前向欧拉 (Forward Euler)

**步长**：Δt = 100ns (= 1/100MHz)

```
// 每 100ns 执行一次（200kHz 开关频率下每周期 50 步）

if (pwm_active) begin
    // SW=ON
    i_L_next = i_L + (dt/L) * (Vin - Vout - i_L * R_L);
    v_C_next = v_C + (dt/C) * (i_L - v_C / R_load);
end else begin
    // SW=OFF
    i_L_next = i_L + (dt/L) * (-Vf - Vout - i_L * R_L);
    v_C_next = v_C + (dt/C) * (i_L - v_C / R_load);
end
```

**数值稳定性验证**：

对于 Buck 变换器，电气时间常数 τ = L/R ≈ 10μH/10Ω ≈ 1μs。
步长 Δt = 100ns = 0.1τ，前向欧拉误差 < 1%，满足仿真精度。

**钳位保护**：
```verilog
// i_L 不能为负（MOSFET/二极管特性）
if (i_L_next[31])  // 符号位 = 1, 即负数
    i_L_next = 32'h00000000;

// v_C 不能为负
if (v_C_next[31])
    v_C_next = 32'h00000000;

// i_L 饱和上限 (防止溢出)
if (i_L_next > IL_MAX)
    i_L_next = IL_MAX;
```

### 2.3.3 定点数格式

| 信号 | 格式 | 位宽 | 单位 | 最大范围 |
|------|------|------|------|----------|
| i_L | 有符号 Q16.16 | 32 bit | A | ±32767A (实际用 0~+10A) |
| v_C (Vout) | 无符号 Q16.16 | 32 bit | V | 0~65535V (实际用 0~+25V) |
| Vin | 无符号 Q16.16 | 32 bit | V | 同上 |
| L | 无符号 Q8.24 | 32 bit | μH | 0~255μH |
| C | 无符号 Q8.24 | 32 bit | μF | 0~255μF |
| R_load | 无符号 Q16.16 | 32 bit | Ω | 0~65535Ω |
| R_L | 无符号 Q8.24 | 32 bit | mΩ | 0~255Ω |
| Vf | 无符号 Q16.16 | 32 bit | V | 二极管正向压降 |
| dt/L | 无符号 Q8.24 | 32 bit | 预计算常数 | — |
| dt/C | 无符号 Q8.24 | 32 bit | 预计算常数 | — |

### 2.3.4 参数预计算

PS 端写入参数时，PL 端自动计算 `dt/L` 和 `dt/C`：

```verilog
// dt = 100ns = 1e-7 s
// L 以 μH 存储 → L_si = L_reg * 1e-6
// dt/L = 1e-7 / (L_reg * 1e-6) = 0.1 / L_reg

// Q8.24: 0.1 → 0x01999999 (≈ 0.1 in Q8.24)
assign dt_over_L = (32'h01999999) / L_reg;  // 除法器，1 cycle latency
assign dt_over_C = (32'h01999999) / C_reg;
```

**实际实现**：使用预计算表而非运行时除法。PS 端在写入参数时计算好 `dt/L` 和 `dt/C`，写入对应寄存器。PL 只负责乘加。

### 2.3.5 求解器流水线

```
Cycle 0: 读取 pwm_active, param_snapshot
Cycle 1: 乘法1: di = dt_over_L * (Vin - Vout - i_L*R_L)
Cycle 2: 乘法2: dv = dt_over_C * (i_L - v_C/R_load)
Cycle 3: 加法: i_L_next = i_L + di
        加法: v_C_next = v_C + dv
Cycle 4: 钳位检查, 更新寄存器, 输出到 DAC 接口
```

**优化**：对于 100ns 步长 @ 100MHz，每个周期 10ns，4 周期 = 40ns < 100ns → 有余量。

如果后续需要更高吞吐（如多相），可将乘加流水线展开：

```verilog
// 流水线乘法器 (使用 DSP48)
// Stage 1: 输入寄存器
// Stage 2: 乘法
// Stage 3: 累加
// 每个 DSP48 可在一个时钟周期完成一次乘加
// 关键路径: DSP48 M-Reg → P-Reg, < 2ns @ ZU3EG speedgrade -1
```

### 2.3.6 接口定义

```verilog
module buck_solver #(
    parameter IL_MAX = 32'h000A0000  // 10.0 A (Q16.16)
) (
    input  wire        clk,           // 100 MHz (clk_pl_100)
    input  wire        rst_n,

    // PWM 输入 (来自 pwm_capture, clk_pl_100 域)
    input  wire [31:0] duty_q16_in,
    input  wire        duty_valid_in,

    // 参数输入 (来自 AXI 寄存器, 双缓冲)
    input  wire [31:0] vin,
    input  wire [31:0] l_val,         // dt/L 预计算值
    input  wire [31:0] c_val,         // dt/C 预计算值
    input  wire [31:0] r_load,
    input  wire [31:0] r_l,
    input  wire [31:0] vf,
    input  wire        param_update,  // 参数更新脉冲（PWM 边界对齐）

    // 输出
    output wire [31:0] v_out,         // Vout, Q16.16
    output wire [31:0] i_l_out,       // I_L, Q16.16
    output wire        pwm_active_out, // 当前开关状态（诊断用）
    output wire        step_valid      // 每个求解步长后产生一个脉冲
);
```

### 2.3.7 PWM 生成 (内部)

求解器内部需要根据 `duty_q16` 生成自己的 PWM 信号（用于判断 SW=ON/OFF）：

```verilog
// 周期计数器：200kHz → 500 cycle @ 100MHz
// 注：周期长度由 PS 配置的期望频率决定
reg  [15:0] pwm_counter;
wire        pwm_active;

// pwm_counter 从 0 计数到 period-1
// period = clk_freq / f_sw = 100e6 / 200e3 = 500
// duty_cycles = duty_q16 * period → (duty_q16 * period) >> 16

wire [31:0] duty_product = duty_q16_in * 16'd500;  // period = 500
wire [15:0] duty_threshold = duty_product[31:16];   // 取高 16 位

assign pwm_active = (pwm_counter < duty_threshold);
```

---

## 2.4 DAC 接口 (`dac_interface`)

### 2.4.1 DAC80508 特性

| 参数 | 值 |
|------|-----|
| 分辨率 | 16 bit |
| 通道数 | 8 |
| 接口 | SPI (3线), CS + SCLK + SDI |
| 最大 SCLK | 50 MHz |
| 输出范围 | 0~5V (内部 REF=2.5V, gain=2 模式) |
| 建立时间 | 5 μs (typ) |
| INL | ±1 LSB max |
| 内置输出缓冲 | 是 (±15mA) |
| 帧格式 | **32-bit**: {8-bit CMD, 8-bit ADDR, 16-bit DATA} |
| 更新模式 | LDAC 低有效同步更新 (可接 GND 设自动更新) |

### 2.4.2 SPI 命令格式

DAC80508 使用 32-bit SPI 帧，不同于 AD5686 的 24-bit：

```
┌──────────┬──────────┬──────────────────┐
│ CMD (8b) │ ADDR (8b)│ DATA (16b,大端)   │
│ [31:24]  │ [23:16]  │ [15:0]            │
└──────────┴──────────┴──────────────────┘

写 DAC 通道 N:
  CMD  = 0x80 | (N << 4)  或直接: 0x08 + N (寄存器地址)
  ADDR = 0x00 (don't care during write)
  DATA = 16-bit DAC code

例: 写 VOUT0 = 0x8000 (半量程 = 2.5V)
  SPI 帧 = {0x08, 0x00, 0x80, 0x00}
           ^DAC0   ^addr  ^DATA_H ^DATA_L

单次传输: 32 cycles @ 50MHz = 640 ns
2 通道更新: 2 × 640 ns = 1.28 μs (vs AD5686: 2 × 480 ns = 960 ns)
```

与 AD5686 对比:

| 项目 | AD5686 (旧) | DAC80508 (新) |
|------|:-----------:|:------------:|
| 帧长 | 24-bit | 32-bit |
| 帧组成 | 4 CMD + 4 ADDR + 16 DATA | 8 CMD + 8 ADDR + 16 DATA |
| 单次传输 @50MHz | 480 ns | 640 ns |
| 2ch 更新时间 | 960 ns | 1.28 μs |
| DAC 数据寄存器地址 | 0x0~0x3 (ch0~3) | 0x08~0x0F (ch0~7) |

### 2.4.3 模块接口

```verilog
module dac_interface #(
    parameter SPI_CLK_DIV = 2  // 100MHz / 2 = 50MHz SCLK
) (
    input  wire        clk,           // 100 MHz (clk_pl_100)
    input  wire        rst_n,

    // 数据输入 (来自求解器)
    // DAC80508 输出 0~5V, 增益×2.4 → 12V
    // DAC code = Vout / 12.0 * 65535 * (5/12) ... 见电压缩放
    input  wire [15:0] ch0_data,      // Vout (DAC code for 0~5V output)
    input  wire [15:0] ch1_data,      // I_L
    input  wire [15:0] ch2_data,      // 预留
    input  wire [15:0] ch3_data,      // 预留
    input  wire [15:0] ch4_data,      // 预留
    input  wire [15:0] ch5_data,      // 预留
    input  wire [15:0] ch6_data,      // 预留
    input  wire [15:0] ch7_data,      // 预留
    input  wire        update_strobe, // 更新触发 (每 5~10 求解步长一次)

    // SPI 物理接口
    output wire        spi_sclk,
    output wire        spi_sdi,     // DAC80508 用 SDI (非 MOSI)
    output wire        spi_cs_n,    // 低有效
    output wire        spi_ldac_n   // 低有效同步更新 (可固定接 GND)
);
```

### 2.4.4 状态机

```
         ┌─────────┐
    ────►│  IDLE   │
         └────┬────┘
              │ update_strobe
              ▼
         ┌─────────┐
         │ CH0_TX  │ ── 发送 32-bit CH0 数据: {0x08, 0x00, DATA_H, DATA_L}
         └────┬────┘
              │
              ▼
         ┌─────────┐
         │ CH1_TX  │ ── 发送 32-bit CH1 数据: {0x09, 0x00, DATA_H, DATA_L}
         └────┬────┘
              │
              ▼
         .......  (CH2~CH7 预留，通过寄存器使能跳过)
              │
              ▼
         ┌─────────┐
         │  WAIT   │ ── CS 拉高，等待 update_strobe 清零
         └────┬────┘
              │ update_strobe == 0
              ▼
           [IDLE]
```

32-bit 帧时序与 24-bit 的区别: 每个通道多发 8 个 bit (1 字节)，状态机的 bit 计数器从 24 改为 32。

### 2.4.5 电压缩放

求解器输出的 `v_C` 是 Q16.16 格式的实际电压值。DAC80508 输出 0~5V (gain=2 模式)，经运放 ×2.4 → 0~12V。

```
Vout (0~12V) → DAC code (0~65535 → 0~5V):
  DAC code = Vout / 12.0 * (65535 / 2.4)           // 或:
  DAC code = Vout / 5.0 * 65535                     // 更直接: DAC 直接输出 0~5V
  DAC code = Vout_Q16 * 65535 / (5 << 16)           // 定点: Vout_Q16 >> 16 * 65535 / 5

预计算缩放因子 (PS 端写入寄存器):
  dac_scale_vout = (65535.0 / 5.0) 的 Q16.16  →  0x000CCCCC (≈ 13107)
  PL 端: dac_code = (v_out[31:16] * dac_scale) >> 16

// Verilog:
wire [31:0] scale_product = v_out[31:16] * DAC_SCALE_REG;  // 16b × 16b = 32b
wire [15:0] dac_code_vout = scale_product[31:16];           // 取高 16 位
```

### 2.4.6 DAC80508 初始化

上电后需通过 SPI 配置寄存器：

| 步骤 | 操作 | SPI 帧 | 说明 |
|------|------|--------|------|
| 1 | 复位 DAC | {0x0A, 0x00, 0x00, 0x01} | 写 SYNC 寄存器 bit0=1 触发软复位 |
| 2 | 配置 gain=2 | {0x04, 0x00, 0x00, 0x01} | 写 GAIN 寄存器: ch0 gain=2 |
| 3 | 配置 ch1 gain=2 | {0x04, 0x01, 0x00, 0x01} | 写 GAIN 寄存器: ch1 gain=2 |
| 4 | 使能内部 REF | {0x03, 0x00, 0x00, 0x01} | 写 CONFIG 寄存器: REF_EN=1 |

初始化由 PS 端在启动时通过 AXI-Lite 寄存器触发，PL 内部状态机自动完成 4 步配置序列。

---

## 2.5 数据捕获 (`capture_manager`)

### 2.5.1 功能

- 环形缓冲区：BRAM 8K × 64-bit
- 每点存储：{Vout[16], IL[16], duty[16], timestamp[16]}
- 触发模式：上升沿/下降沿/电平/软件
- 触发后抓取 4096 点（pre-trigger 512 点）
- 通过 AXI-Stream DMA 传输到 PS DDR

### 2.5.2 接口

```verilog
module capture_manager #(
    parameter BUFFER_DEPTH = 8192,
    parameter PRE_TRIGGER  = 512,
    parameter CAPTURE_SIZE = 4096
) (
    input  wire        clk,
    input  wire        rst_n,

    // 数据输入
    input  wire [15:0] vout,
    input  wire [15:0] il,
    input  wire [15:0] duty,
    input  wire        step_valid,   // 每个求解步长一次

    // 触发配置 (AXI 寄存器)
    input  wire [1:0]  trig_src,     // 0=软件 1=上升沿 2=下降沿 3=电平
    input  wire [15:0] trig_level,
    input  wire        trig_arm,     // 使能触发

    // 状态输出
    output wire        trig_occurred,

    // AXI-Stream 输出 (到 DMA)
    output wire [63:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast
);
```

---

## 2.7 约束文件示例 (`zu3eg.xdc`)

```tcl
# 时钟约束
create_clock -period 2.5  -name clk_pl_400  [get_ports clk_pl_400_p]
create_clock -period 10.0 -name clk_pl_100  [get_ports clk_pl_100_p]
create_clock -period 10.0 -name clk_axi     [get_ports m_axi_gp0_aclk]

# 跨域路径
set_clock_groups -asynchronous \
    -group [get_clocks clk_pl_400] \
    -group [get_clocks clk_pl_100] \
    -group [get_clocks clk_axi]

# PWM 输入 (3.3V LVCMOS)
set_property PACKAGE_PIN TBD [get_ports pwm_in]
set_property IOSTANDARD LVCMOS33 [get_ports pwm_in]

# DAC SPI (3.3V LVCMOS) — DAC80508
set_property PACKAGE_PIN TBD [get_ports dac_sclk]
set_property PACKAGE_PIN TBD [get_ports dac_sdi]
set_property PACKAGE_PIN TBD [get_ports dac_cs_n]
set_property PACKAGE_PIN TBD [get_ports dac_ldac_n]
set_property IOSTANDARD LVCMOS33 [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]

# 输出延迟约束 (DAC SPI)
set_output_delay -clock [get_clocks clk_pl_100] -max 2.0 [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]
set_output_delay -clock [get_clocks clk_pl_100] -min 1.0 [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]

# 输入延迟约束 (PWM)
set_input_delay -clock [get_clocks clk_pl_400] -max 2.0 [get_ports pwm_in]
set_input_delay -clock [get_clocks clk_pl_400] -min 1.0 [get_ports pwm_in]
```
