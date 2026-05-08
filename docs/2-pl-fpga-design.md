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

### 2.4.1 AD5686 特性

| 参数 | 值 |
|------|-----|
| 分辨率 | 16 bit |
| 通道数 | 4 |
| 接口 | SPI (3线/4线) |
| 最大 SCLK | 50 MHz |
| 输出范围 | 0 ~ 2.5V (内部基准) |
| 建立时间 | 5 μs (typ) |
| 命令格式 | 24-bit: {4-bit CMD, 4-bit ADDR, 16-bit DATA} |

### 2.4.2 SPI 命令

```
WRITE_TO_INPUT_REG:   CMD = 0001
UPDATE_DAC:           CMD = 0010
WRITE_AND_UPDATE:     CMD = 0011
```

单次 DAC 更新时序（WRITE_AND_UPDATE, Channel A）:
```
24-bit 帧: 0011_0000_D15_D14_..._D0 = 24 cycles @ 50MHz ≈ 480 ns
```

### 2.4.3 模块接口

```verilog
module dac_interface #(
    parameter SPI_CLK_DIV = 2  // 100MHz / 2 = 50MHz SCLK
) (
    input  wire        clk,           // 100 MHz (clk_pl_100)
    input  wire        rst_n,

    // 数据输入 (来自求解器)
    input  wire [15:0] ch0_data,      // Vout (已量化为 0~65535 → 0~2.5V)
    input  wire [15:0] ch1_data,      // I_L
    input  wire [15:0] ch2_data,      // 预留
    input  wire [15:0] ch3_data,      // 预留
    input  wire        update_strobe, // 更新触发 (每 5~10 求解步长一次)

    // SPI 物理接口
    output wire        spi_sclk,
    output wire        spi_mosi,
    output wire        spi_cs_n    // 低有效
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
         │ CH0_TX  │ ── 发送 24-bit CH0 数据
         └────┬────┘
              │
              ▼
         ┌─────────┐
         │ CH1_TX  │ ── 发送 24-bit CH1 数据
         └────┬────┘
              │
              ▼
         ┌─────────┐
         │ CH2_TX  │ ── 预留
         └────┬────┘
              │
              ▼
         ┌─────────┐
         │ CH3_TX  │ ── 预留
         └────┬────┘
              │
              ▼
         ┌─────────┐
         │  WAIT   │ ── CS 拉高，等待 update_strobe 清零
         └────┬────┘
              │ update_strobe == 0
              ▼
           [IDLE]
```

### 2.4.5 电压缩放

求解器输出的 `v_C` 是 Q16.16 格式的实际电压值。DAC 输入需要 16-bit 无符号数（0~65535 → 0~2.5V）。

```verilog
// 电压缩放: Vout (Q16.16) → DAC code
// 假设 Vout 范围 0~12V, DAC 输出 0~2.5V (后经运放放大 ×4.8 得到 0~12V)
// DAC code = Vout / 12.0 * 65535  （Q16.16 除法）
// 简化: Vout_Q16 * 65535 / (12 << 16)

wire [47:0] scale_product = v_out[31:0] * 48'd65535;  // 48-bit 乘法
wire [15:0] dac_code_vout = scale_product[47:16] / 12; // 除以 12（取整近似）
```

实际实现用预计算的缩放因子寄存器避免运行时除法。

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

# DAC SPI (3.3V LVCMOS)
set_property PACKAGE_PIN TBD [get_ports dac_sclk]
set_property PACKAGE_PIN TBD [get_ports dac_mosi]
set_property PACKAGE_PIN TBD [get_ports dac_cs_n]
set_property IOSTANDARD LVCMOS33 [get_ports {dac_sclk dac_mosi dac_cs_n}]

# 输出延迟约束 (DAC SPI)
set_output_delay -clock [get_clocks clk_pl_100] -max 2.0 [get_ports {dac_sclk dac_mosi dac_cs_n}]
set_output_delay -clock [get_clocks clk_pl_100] -min 1.0 [get_ports {dac_sclk dac_mosi dac_cs_n}]

# 输入延迟约束 (PWM)
set_input_delay -clock [get_clocks clk_pl_400] -max 2.0 [get_ports pwm_in]
set_input_delay -clock [get_clocks clk_pl_400] -min 1.0 [get_ports pwm_in]
```
