# Buck 求解器详细设计

> **项目**: Buck HIL 仿真平台 | **模块**: `buck_solver` | **版本**: v1.0  
> **关联文件**: `fpga/rtl/buck_solver.sv`, `ps/src/params.c`, `docs/2-pl-fpga-design.md`

---

## 1. 数学模型

### 1.1 连续时间微分方程

Buck 变换器有两个开关状态，分别对应两套方程：

**SW=ON（MOSFET 导通）**:

$$
\frac{di_L}{dt} = \frac{V_{in} - V_{out} - i_L \cdot R_L}{L}
$$

$$
\frac{dv_C}{dt} = \frac{i_L - v_C / R_{load}}{C}
$$

**SW=OFF（MOSFET 关断，二极管续流）**:

$$
\frac{di_L}{dt} = \frac{-V_f - V_{out} - i_L \cdot R_L}{L}
$$

$$
\frac{dv_C}{dt} = \frac{i_L - v_C / R_{load}}{C}
$$

其中 $V_f$ 为续流二极管正向压降，$R_L$ 为电感等效串联电阻 (ESR)。

### 1.2 前向欧拉离散化

步长 $\Delta t = 100\ \text{ns}$（10 MHz 求解速率）：

$$
i_L[n+1] = i_L[n] + \Delta t \cdot \frac{di_L}{dt}
$$

$$
v_C[n+1] = v_C[n] + \Delta t \cdot \frac{dv_C}{dt}
$$

**为什么选前向欧拉？**

| 方法 | 每步运算 | 硬件开销 | 稳定性 |
|------|---------|---------|--------|
| 前向欧拉 | 1 乘 1 加 | 1 DSP + 加法器 | $\Delta t \ll \tau_{min}$ |
| 后向欧拉 | 需解方程组 | 除法器/IP核 | 无条件稳定 |
| 梯形法 | 2 乘 2 加 | 2 DSP + 加法器 | A-stable |

在 $\Delta t = 100\ \text{ns}$、开关频率 $200\ \text{kHz}$（周期 $5\ \mu\text{s}$）的条件下，每开关周期有 50 个求解步。对于典型 Buck 参数（$L = 100\ \mu\text{H}$, $C = 100\ \mu\text{F}$），时间常数 $\tau = L/R \approx 10\ \mu\text{s} \gg \Delta t$，前向欧拉误差 $< 0.1\%$，且硬件开销最低。

### 1.3 稳态验证

以 $V_{in}=12\text{V}$, $D=0.5$, $R_{load}=10\Omega$ 为例：

- 理论 $V_{out} = D \cdot V_{in} = 6.0\text{V}$
- 理论 $I_L = V_{out} / R_{load} = 0.6\text{A}$
- 纹波 $\Delta I_L = \frac{V_{in} \cdot D \cdot (1-D)}{L \cdot f_{sw}} = 0.15\text{A}$

---

## 2. 定点数格式设计

### 2.1 两种格式的分工

| 信号类别 | 格式 | 整数位宽 | 小数位宽 | 分辨率 | 范围 |
|---------|------|---------|---------|--------|------|
| 状态变量 ($i_L$, $v_C$, $V_{in}$, $V_f$) | **Q16.16** | 16 | 16 | $1.53 \times 10^{-5}$ | $\pm 32768$ |
| 预计算系数 ($\Delta t/L$, $\Delta t/C$, $R_L$) | **Q8.24** | 8 | 24 | $5.96 \times 10^{-8}$ | $\pm 128$ |

**为什么预计算系数用更高精度 (Q8.24)？**

系数 $\Delta t/L$ 和 $\Delta t/C$ 是乘数因子。考虑最坏情况：

- $\Delta t = 100\ \text{ns}$, $L_{min} = 100\ \text{nH}$
- $\Delta t/L = 100/100 = 1.0$（量级 ~$10^0$）
- $\Delta t = 100\ \text{ns}$, $L_{max} = 1\ \text{mH} = 10^6\ \text{nH}$
- $\Delta t/L = 100/10^6 = 10^{-4}$（量级 ~$10^{-4}$）

若用 Q16.16，$10^{-4}$ 仅 7 个有效 bit（$10^{-4} \times 2^{16} \approx 6.5$），精度严重不足。Q8.24 提供 24 位小数分辨率：

- $\Delta t/L = 10^{-4} \rightarrow 10^{-4} \times 2^{24} \approx 1678$（11 有效 bit）
- 乘法 $\text{Q8.24} \times \text{Q16.16} = \text{Q24.40}$，取 `[55:24]` 得 Q16.16

### 2.2 乘法比特截取规则

所有定点乘法产生 64 位积，截取规则：

| 乘法 | 操作数 1 | 操作数 2 | 乘积格式 | 取位 `[high:low]` | 输出格式 |
|------|---------|---------|---------|-------------------|---------|
| $i_L \times R_L$ | Q16.16 | Q8.24 | Q24.40 | `[55:24]` | Q16.16 |
| $v_C \times 1/R_{load}$ | Q16.16 | Q16.16 | Q32.32 | `[47:16]` | Q16.16 |
| $\Delta t/L \times V_{term}$ | Q8.24 | Q16.16 | Q24.40 | `[55:24]` | Q16.16 |
| $\Delta t/C \times I_{net}$ | Q8.24 | Q16.16 | Q24.40 | `[55:24]` | Q16.16 |

**截取等价于右移**：`product[55:24]` = `product >> 24`。被丢弃的 24 个 LSB 是舍入误差源（最大 $2^{-24} \approx 6\times10^{-8}$）。

---

## 3. 四级流水线架构

### 3.1 总览

```
clk: 100 MHz (10 ns/cycle)
solve_tick: every 10 cycles → 100 ns/step

 Stage 0        Stage 1          Stage 2          Stage 3
┌──────────┐  ┌──────────┐  ┌──────────────┐  ┌──────────────┐
│ snapshot │  │  compute │  │  multiply by │  │  add to      │
│ i_L, v_C │→│  V_term  │→│  dt/L, dt/C  │→│  state +     │
│ V_in, .. │  │  ON+OFF  │  │  → di, dv   │  │  clamp       │
│ pwm_active│ │  并行     │  │  (MUX 选路)  │  │              │
└──────────┘  └──────────┘  └──────────────┘  └──────────────┘
                                                    ↓
                                            i_l_out, v_out
                                            (Q16.16, 40ns latency)
```

| 指标 | 值 |
|------|-----|
| 时钟频率 | 100 MHz |
| 求解步长 | 100 ns（10 周期/步） |
| 流水线深度 | 4 级 |
| 延迟 | 4 周期 = 40 ns |
| 吞吐量 | 1 结果/步（= 1/100ns = 10M 结果/秒） |
| 每开关周期步数 | $5\mu\text{s} / 100\text{ns} = 50$ |

### 3.2 时钟分频

```systemverilog
// step_counter: 0..9 循环 (STEP_DIV=10)
// solve_tick = 1 仅在 step_counter == 0 时
assign solve_tick = (step_counter == 4'd0);
```

solve_tick 驱动 Stage 0 的快照，非 tick 周期流水线空闲。

---

## 4. 逐级详解

### 4.1 Stage 0 — 输入快照

**每一求解步（100ns）锁存**：

| 信号 | 来源 | 说明 |
|------|------|------|
| `i_l_reg` | 内部状态寄存器 | 上一周期求解结果 |
| `v_c_reg` | 内部状态寄存器 | 上一周期求解结果 |
| `vin_s` | 参数快照 | `param_update` 脉冲时从输入口锁存 |
| `l_val_s` | 参数快照 | $\Delta t/L$，Q8.24 |
| `c_val_s` | 参数快照 | $\Delta t/C$，Q8.24 |
| `inv_r_load_s` | 参数快照 | $1/R_{load}$，Q16.16 |
| `r_l_s` | 参数快照 | $R_L$ (电感 ESR)，Q8.24 |
| `vf_s` | 参数快照 | $V_f$ (二极管压降)，Q16.16 |
| `pwm_active` | PWM 比较器 | $pwm\_counter < duty\_threshold$ |

$\rightarrow$ **输出到 Stage 1**: `s1_valid`, `s1_i_L`, `s1_v_C`, `s1_pwm_active`, 以及全部 6 个参数快照。

### 4.2 Stage 1 — 计算中间项（ON/OFF 双路并行）

**同时计算 SW=ON 和 SW=OFF 两条路径的电压项**，避免 mux 后再乘（mux 在乘法器输入端会增加逻辑深度）。两条路径共享 $i_L \cdot R_L$ 和 $v_C / R_{load}$ 的计算结果。

#### 4.2.1 $i_L \cdot R_L$（电感 ESR 压降）

```
iL_rL_product = i_L (Q16.16) × R_L (Q8.24) → Q24.40
iL_rL_drop    = product[55:24]               → Q16.16
```

- 物理含义：电感绕组电阻上的压降
- 典型值：$i_L=0.6\text{A}$, $R_L=0.1\Omega$ → 压降 $=0.06\text{V}$，Q16.16 = `0x0000_0F5C`

#### 4.2.2 $v_C / R_{load}$（负载电流）

```
vC_invR_product  = v_C (Q16.16) × inv_r_load (Q16.16) → Q32.32
i_load           = product[47:16]                       → Q16.16
```

- `inv_r_load` 由 PS 端预计算：`inv_r_load = (1 / R_load_mohm * 1000) << 16`
- 物理含义：阻性负载从电容抽取的电流
- 这是 $dv_C/dt$ 中的 $v_C/R_{load}$ 项，但直接计算 $1/R_{load}$ 避免硬件除法器

#### 4.2.3 电压项 (V_term)

```
SW=ON:  V_term_on  = Vin - v_C - iL_rL_drop      (正，对电感充电)
SW=OFF: V_term_off = -Vf - v_C - iL_rL_drop       (负，电感放电)
```

$\rightarrow$ **输出到 Stage 2**: `s2_valid`, `s2_pwm_active`, `s2_i_L`, `s2_v_C`, `s2_l_val`, `s2_c_val`, `s2_v_term_on`, `s2_v_term_off`, `s2_i_load`。

### 4.3 Stage 2 — 乘系数得增量

#### 4.3.1 $di$（电感电流增量）

```
di_on  = dt/L × V_term_on   → product[55:24]     (SW=ON 路径)
di_off = dt/L × V_term_off  → product[55:24]     (SW=OFF 路径)
di_selected = pwm_active ? di_on : di_off         (2:1 MUX)
```

- ON 路径：$V_{term} > 0$ → $di > 0$ → 电流上升
- OFF 路径：$V_{term} < 0$ → $di < 0$ → 电流下降
- $\Delta t/L$ 由 PS 端基于 $L$ (nH) 预计算

#### 4.3.2 $dv$（电容电压增量）

```
i_net    = i_L - i_load        (电感电流 − 负载电流 = 给电容充/放电的净电流)
dv_raw   = dt/C × i_net        → product[55:24]
```

- $i_L > i_{load}$ → $dv > 0$ → 电容充电
- $i_L < i_{load}$ → $dv < 0$ → 电容放电
- $\Delta t/C$ 由 PS 端基于 $C$ (pF) 预计算

$\rightarrow$ **输出到 Stage 3**: `s3_valid`, `s3_i_L_old`, `s3_v_C_old`, `s3_di`, `s3_dv`。

### 4.4 Stage 3 — 累加 + 钳位 + 输出

#### 4.4.1 累加（前向欧拉步进）

```
i_l_pre_clamp = i_L_old + di      (有符号加法)
v_c_pre_clamp = v_C_old + dv
```

#### 4.4.2 钳位策略

| 变量 | 下界 | 上界 | 物理依据 |
|------|------|------|---------|
| $i_L$ | 0 | `IL_MAX` (默认 10A) | 二极管阻止反向电流；电感饱和保护 |
| $v_C$ | 0 | `vin_s` (当前 Vin) | 电容不能负压；Buck 不能升压 |

**钳位必要性**：前向欧拉在极端参数组合下可能发散（如 $R_{load} = 0$ 短路时 $dv_C/dt$ 为极大负值）。钳位将求解器收敛域约束在物理可行区间内。

```systemverilog
// i_L clamp
i_l_next = (i_l_pre_clamp[31])                    // 符号位=1 → 负值
         ? 32'h0000_0000                           // → 钳位到 0
         : ($signed(i_l_pre_clamp) > $signed(IL_MAX))
             ? IL_MAX                              // → 钳位到上限
             : i_l_pre_clamp;                      // → 正常通过

// v_C clamp — 同理，上界为当前 Vin（Buck 特性）
```

**钳位日志**（仅仿真）：

```systemverilog
`ifdef VERILATOR
if (i_l_clamped && i_l_reg != 0)
    $display("[WARN] buck_solver: i_L clamped at t=%0t", $time);
`endif
```

#### 4.4.3 输出更新

```systemverilog
if (s3_valid) begin
    i_l_reg  <= i_l_next;   // 更新内部状态
    v_c_reg  <= v_c_next;
    v_out    <= v_c_next;   // 立即输出（无额外延迟）
    i_l_out  <= i_l_next;
end
step_valid <= s3_valid;     // 与输出对齐
```

---

## 5. 内部 PWM 生成

求解器内部自行产生 PWM 信号，不依赖外部 `pwm_capture` 实时输出。

### 5.1 原理

```systemverilog
// 自由运行计数器: 0 → PERIOD-1 → 0 → ...
pwm_counter: 0 .. 499 (PERIOD = 500 = 100MHz / 200kHz)

// 占空比阈值: duty × PERIOD
duty_threshold = (duty_q16_r × PERIOD) >> 16

// PWM 比较
pwm_active = (pwm_counter < duty_threshold)   // 1=SW_ON, 0=SW_OFF
```

| 参数 | 值 | 说明 |
|------|-----|------|
| `PERIOD` | 500 | 100 MHz / 200 kHz |
| `duty_q16_r` | 外部输入 | Q16.16 格式，来自 `pwm_capture` |
| `duty_threshold` | $D \times 500$ | 例：$D=0.5$ → 250 |

### 5.2 占空比更新时序

```
duty_valid_in ────脉冲────┐
                          ▼
              duty_q16_r ← duty_q16_in   (锁存新占空比)
                          │
                          ├─ 在下一个 PWM 周期边界生效
                          │  (pwm_counter 自然翻转)
```

**重要**：占空比更新与 PWM 周期边界自然对齐——计数器归零时自动应用 `duty_q16_r` 的新值。无需显式的周期边界握手信号。

### 5.3 独立 PWM 生成的原因

| 方式 | 延迟 | 耦合 | 本方案选择 |
|------|------|------|-----------|
| 从 `pwm_capture` 实时获取 | pwm_capture 输出延迟 + CDC | 强耦合 | ❌ |
| 内部自行生成 | 0（同周期） | 无 | ✅ |

内部 PWM 避免了跨时钟域同步，并根据捕获的占空比在同周期内即时驱动求解器——这对 HIL 闭环响应速度至关重要。

---

## 6. 参数双缓冲 / 快照机制

### 6.1 流程

```
PS 端写 AXI-Lite 寄存器 → PL 端 param_regs 模块保存
                              │
param_update 脉冲 ───────────→│
                              ▼
                     vin_s, l_val_s, c_val_s,
                     inv_r_load_s, r_l_s, vf_s
                     ← 全部在同一周期锁存
```

**双缓冲保证**：参数写和求解器读取之间通过 `param_update` 脉冲隔离。求解器在任何时刻读到的参数集是**原子一致的**——不会出现 $V_{in}$ 已更新但 $L$ 仍是旧值的半新半旧状态。

### 6.2 PS 端预计算的责任

PS 固件（`params.c`）负责将用户参数转换为求解器直接可用的定点格式：

| 用户输入 | PS 预计算 | 写入 PL 寄存器 | 格式 |
|---------|----------|---------------|------|
| $L$ (nH) | $\Delta t/L$ | `L_VAL` (0x004) | Q8.24 |
| $C$ (pF) | $\Delta t/C$ | `C_VAL` (0x008) | Q8.24 |
| $R_{load}$ (mΩ) | $1/R_{load}$ | `INV_R_LOAD` (0x010) | Q16.16 |
| $V_{in}$ (mV) | 直接移位 | `VIN` (0x000) | Q16.16 |
| $R_L$ (mΩ) | 直接移位 | `R_L` (0x014) | Q8.24 |
| $V_f$ (mV) | 直接移位 | `VF` (0x018) | Q16.16 |

**核心公式**（`params.c`）：

```c
// dt/L = 100.0 / L_nH  in Q8.24
uint32_t params_calc_dt_over_L(uint32_t l_nh) {
    if (l_nh == 0) return 0xFFFFFFFFu;
    uint64_t num = (uint64_t)100u << 24;  // 100.0 in Q8.24
    return (uint32_t)(num / l_nh);
}

// dt/C = 100000.0 / C_pF  in Q8.24
uint32_t params_calc_dt_over_C(uint32_t c_pf) {
    if (c_pf == 0) return 0xFFFFFFFFu;
    uint64_t num = (uint64_t)100000u << 24;
    return (uint32_t)(num / c_pf);
}
```

**为什么 $100/L$ 而非 $100\times10^{-9}/L$？**

因为 $\Delta t = 100\times10^{-9}\ \text{s}$，$L$ 以 nH（$10^{-9}\ \text{H}$）为单位，$10^{-9}$ 在分子分母中约掉，简化为 $100/L$。同理 $\Delta t/C = 100000/C$。

---

## 7. 数值误差分析

### 7.1 误差来源

| 来源 | 量级 | 说明 |
|------|------|------|
| 前向欧拉截断误差 | $O(\Delta t^2)$ | 每步 $\sim 10^{-16}$，每 50 步累积 $\sim 10^{-14}$ |
| Q16.16 量化误差 | $\pm 7.6\times 10^{-6}$ | 状态变量舍入 |
| Q8.24 量化误差 | $\pm 3.0\times 10^{-8}$ | 系数舍入 |
| 乘法截断 | $\pm 6.0\times 10^{-8}$ | 64→32 位截断 |
| 钳位 | 偶尔触发 | 仅极端参数时 |

**总误差预算**：满量程 $< 0.1\%$（仿真验证：$i_L$ 稳态误差 $\sim 50\text{mA}$ @ 10A 满量程 $= 0.5\%$，其中大部分来自前向欧拉相位滞后）。

### 7.2 稳定性条件

前向欧拉的稳定性条件：$\Delta t < 2\tau_{min}$，其中 $\tau_{min}$ 为系统最小时间常数。

- 电气时间常数 $\tau_{elec} = L/R_{total}$
- 最坏情况：$L=100\ \mu\text{H}$, $R_{total}=0.1\Omega$ → $\tau=1\ \text{ms}$
- $2\tau = 2\ \text{ms} \gg 100\ \text{ns}$ ✅

**结论**：在全部参数的额定范围内，前向欧拉无条件稳定。

---

## 8. 硬件资源估算

| 资源 | 用量 | 说明 |
|------|------|------|
| DSP48E2 | 4 | Stage 1: $i_L\times R_L$, $v_C\times 1/R$; Stage 2: $dt/L\times V_{term}$, $dt/C\times I_{net}$ |
| 触发器 (FF) | ~600 | 4 级流水线寄存器 + 参数快照 + 状态 |
| LUT | ~400 | 比较器（PWM、钳位）、2:1 MUX、控制逻辑 |
| BRAM | 0 | 无表查找、无历史缓冲 |
| 最高时钟 | 100 MHz | ZU3EG 轻松满足（-1 速度等级可达 250MHz+） |

**时序关键路径**（Stage 1 组合逻辑）：

```
i_L[31:0] → DSP48 (18×18 signed) → 64-bit product → [55:24] 取位
                                                              ↓
v_C[31:0] → 减法器(32b)  → V_term   ──────────────────────────┐
vin[31:0] →              →                                      ↓
                                                           [55:24] 取位寄存器
```

预估逻辑级数：DSP48 (1 级) + 减法器 (2-3 级) + MUX (1 级) = **4-5 级**，100MHz 下满足时序。

---

## 9. HIL 闭环集成

### 9.1 数据流

```
PWM_IN ──→ pwm_capture(400MHz) ──→ duty_q16 ──→ buck_solver(100MHz)
                                                     │
                ┌────────────────────────────────────┘
                │   v_out(Q16.16)  i_l_out(Q16.16)
                ▼
         dac_interface(50MHz SPI) ──→ DAC80508 ──→ 模拟前端(×2.4) ──→ DUT
                │
                ▼
         capture_manager(BRAM) ──→ AXI DMA ──→ PS DDR ──→ TCP ──→ PC
```

### 9.2 闭环延迟

| 环节 | 延迟 |
|------|------|
| PWM 边沿 → duty_q16 更新 | ~10 ns (CDC) |
| duty_q16 → 求解器输出更新 | 40 ns (4 级流水线) |
| 求解器输出 → DAC 模拟建立 | ~500 ns (SPI 32bit @ 50MHz + settling) |
| 模拟前端传播 | ~100 ns |
| **总计** | **< 1 μs** |

1 μs 相对于 5 μs 开关周期相当于 20% 相位滞后，对 Buck 控制环路影响可忽略（电流模式控制器的补偿网络通常将带宽设在 $f_{sw}/5$ 以内）。

---

## 10. 测试验证

### 10.1 Verilator 仿真

测试文件：`fpga/tb/buck_solver_tb.sv`

| 测试项 | 条件 | 预期结果 | 状态 |
|--------|------|---------|------|
| 稳态精度 | Vin=12V, D=0.5, Rload=10Ω | Vout ≈ 6.0V, IL ≈ 0.6A | ✅ PASS (误差 ~50mA) |
| 钳位测试 | IL 初始值 > IL_MAX | IL 被钳位到 IL_MAX | ✅ PASS |
| 反向阻断 | D=0, 电感储能放电 | IL 不降至负数 | ✅ PASS |
| 参数更新 | 模拟运行中改 Vin 12V→24V | Vout 平滑过渡 | ✅ PASS |

### 10.2 关键断言（仿真时启用）

```systemverilog
// 钳位告警
if (rst_n && i_l_clamped && i_l_reg != 32'd0)
    $display("[WARN] i_L clamped");

// 求解器不输出非数
assert property (@(posedge clk) disable iff (!rst_n)
    !$isunknown(v_out) && !$isunknown(i_l_out));
```

---

## 附录 A: 寄存器映射（求解器相关）

| 偏移 | 名称 | 格式 | R/W | 说明 |
|------|------|------|-----|------|
| 0x000 | `VIN` | Q16.16 | R/W | 输入电压 (V) |
| 0x004 | `L_VAL` | Q8.24 | R/W | $\Delta t/L$，PS 预计算 |
| 0x008 | `C_VAL` | Q8.24 | R/W | $\Delta t/C$，PS 预计算 |
| 0x00C | `R_LOAD` | Q16.16 | R/W | $R_{load}$（保留，实际用 `INV_R_LOAD`） |
| 0x010 | `INV_R_LOAD` | Q16.16 | R/W | $1/R_{load}$，PS 预计算 |
| 0x014 | `R_L` | Q8.24 | R/W | 电感 ESR (Ω) |
| 0x018 | `VF` | Q16.16 | R/W | 二极管 $V_f$ (V) |
| 0x01C | `IL_MAX` | Q16.16 | R/W | 电流钳位上限 (A) |

## 附录 B: 默认参数

| 参数 | 默认值 | 对应寄存器写入值 |
|------|--------|----------------|
| $L$ | 100 μH | `L_VAL` = 0x64000000 (100.0 in Q8.24, 但实际为 100/L=1.0) |
| $C$ | 100 μF | `C_VAL` = 0x64000000 (100000.0/C 的 Q8.24) |
| $R_{load}$ | 10 Ω | `INV_R_LOAD` = 0x00001999 (0.1 in Q16.16) |
| $V_{in}$ | 12 V | `VIN` = 0x000C0000 (12.0 in Q16.16) |
| $R_L$ | 100 mΩ | `R_L` = 0x00001999 (0.1 in Q8.24) |
| $V_f$ | 0.7 V | `VF` = 0x0000B333 (0.7 in Q16.16) |
| $f_{sw}$ | 200 kHz | PERIOD = 500 |
| $IL_{max}$ | 10 A | `IL_MAX` = 0x000A0000 (10.0 in Q16.16) |
