# Vivado 工程操作指南 — Buck HIL ZU3EG

> **最后更新**: 2026-05-09 | **适用版本**: Vivado 2023.2+ | **目标器件**: XCZU3EG-1SFVC784E

---

## 1. 前提条件

| 项目 | 要求 |
|------|------|
| Vivado | 2023.2 或更高 (含 Zynq UltraScale+ 许可) |
| 操作系统 | Windows 10/11 或 Ubuntu 22.04+ |
| 磁盘空间 | 至少 10 GB (综合+实现产物) |
| 引脚信息 | 你的 PCB / 开发板的引脚分配表 |

---

## 2. 创建工程（一键脚本）

### 2.1 启动 Vivado

```
开始菜单 → Xilinx Design Tools → Vivado 2023.x
```

### 2.2 运行 Tcl 脚本

在 Vivado **Tcl Console** 底部输入框中执行：

```tcl
# 切换到工程脚本目录（按实际路径调整）
cd D:/buck-hil/fpga/vivado

# 执行创建脚本
source create_vivado_project.tcl
```

**脚本自动完成**：
- ✅ 创建工程 `buck_hil` → `fpga/vivado/build/`
- ✅ 添加全部 RTL 源文件（7 个 `.sv`）
- ✅ 添加约束文件 `zu3eg.xdc`
- ✅ 创建 Block Design（ZynqMP PS + AXI Interconnect + DMA）
- ✅ 连接时钟、复位、AXI 总线、中断

### 2.3 打开 GUI（如已关闭）

```tcl
# 方法 1: Vivado 欢迎页 → Open Project → 选择 fpga/vivado/build/buck_hil.xpr
# 方法 2: 命令行
start_gui
open_project fpga/vivado/build/buck_hil.xpr
```

---

## 3. 引脚分配

### 3.1 找到约束文件

在 Vivado **Sources** 窗口 → Constraints → `zu3eg.xdc`

### 3.2 替换 TBD 引脚

文件中有 9 处 `TBD` 需要替换为实际引脚号。对照你的 PCB 原理图：

```tcl
# 示例（实际值根据你的 PCB）:
set_property PACKAGE_PIN H12 [get_ports pwm_in]        ;# 假设接 PMOD JA1
set_property PACKAGE_PIN L14 [get_ports dac_sclk]      ;# 假设接 PMOD JB1
set_property PACKAGE_PIN L13 [get_ports dac_sdi]        ;# 假设接 PMOD JB2
set_property PACKAGE_PIN M13 [get_ports dac_cs_n]       ;# 假设接 PMOD JB3
set_property PACKAGE_PIN N14 [get_ports dac_ldac_n]     ;# 接地 (GND)
set_property PACKAGE_PIN K15 [get_ports {debug_led[0]}] ;# LED0
set_property PACKAGE_PIN J15 [get_ports {debug_led[1]}] ;# LED1
set_property PACKAGE_PIN J18 [get_ports {debug_led[2]}] ;# LED2
set_property PACKAGE_PIN H18 [get_ports {debug_led[3]}] ;# LED3
```

**如果你用的是现成开发板**（如 ZCU104 / Ultra96），直接填对应 PMOD / GPIO 引脚即可。

---

## 4. Block Design 审查

### 4.1 打开 Block Design

Sources 窗口 → `system.bd` → 双击打开 Diagram

### 4.2 检查项

| 检查点 | 确认内容 |
|--------|---------|
| PS (`zynq_ultra_ps_e`) | pl_clk0=100MHz, pl_clk1=400MHz, GP0 enabled, HP0 enabled |
| AXI Interconnect | 1 Master / 1 Slave, 100MHz |
| AXI DMA | S2MM only, 64-bit data width, burst=16 |
| Reset | 两组 proc_sys_reset (100MHz / 400MHz) |
| Concat | 4 输入中断合并 |

### 4.3 顶层封装选择

推荐方式：**在 `buck_hil_top.sv` 中例化 Block Design Wrapper**。

1. 右键 `system.bd` → **Create HDL Wrapper**
2. 选择 **"Let Vivado manage wrapper and auto-update"**
3. 在 `buck_hil_top.sv` 中，把 AXI 接口端口连接到 Wrapper 例化

如果 Block Design 端口与 `buck_hil_top` 的 AXI 端口名不完全匹配，直接在 wrapper 中修改端口连接。

---

## 5. 综合 (Synthesis)

### 5.1 运行

```
Flow Navigator → Synthesis → Run Synthesis
```

或 Tcl：

```tcl
launch_runs synth_1 -jobs 8
wait_on_run synth_1
```

### 5.2 预期结果

- 预计耗时：2-5 分钟
- 资源利用率参考：
  - LUT: < 5%
  - FF: < 3%
  - DSP: 4 个
  - BRAM: 8 个 (capture_manager)

### 5.3 常见警告

| 警告 | 说明 | 处理 |
|------|------|------|
| `[Synth 8-3331] unconnected port` | 未用引脚（ch2~ch7 DAC） | 忽略 |
| `[Synth 8-327] inferring latch` | latch 推断 | 检查 `always_comb` 是否完整覆盖 |
| `[Synth 8-6014] unused sequential element` | 部分 FF 被优化 | 正常 |

---

## 6. 实现 (Implementation)

### 6.1 运行

```
Flow Navigator → Implementation → Run Implementation
```

```tcl
launch_runs impl_1 -jobs 8
wait_on_run impl_1
```

### 6.2 时序检查

实现完成后，检查 Timing Summary：

```
Flow Navigator → Implementation → Open Implemented Design → Report Timing Summary
```

关注：

| 指标 | 预期值 |
|------|--------|
| WNS (最差负裕量) | > 0 ns（无违规） |
| TNS (总负裕量) | 0 ns |
| clk_400 (2.5ns) | WNS > 0.2 ns |
| clk_100 (10ns) | WNS > 5 ns（宽松） |

### 6.3 时序违规处理

如果 clk_400 域出现违规：

```tcl
# 1. 提高综合/实现 effort
set_property STEPS.SYNTH_DESIGN.ARGS.MORE_OPTIONS {directive PerformanceOptimized} [get_runs synth_1]
set_property STEPS.OPT_DESIGN.ARGS.MORE_OPTIONS {directive Explore} [get_runs impl_1]

# 2. CDC 路径设为 false path（已在 xdc 中）
# 3. 降低 pwm_capture 时钟 → 改 PS pl_clk1 为 300MHz
```

---

## 7. 生成 Bitstream

### 7.1 运行

```
Flow Navigator → Program and Debug → Generate Bitstream
```

```tcl
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
```

### 7.2 产物位置

```
fpga/vivado/build/buck_hil.runs/impl_1/buck_hil_top.bit   ← PL bitstream
fpga/vivado/build/buck_hil.runs/impl_1/buck_hil_top.bin    ← 二进制格式
```

---

## 8. 导出到 Vitis（PS 固件开发）

### 8.1 导出 XSA

```
File → Export → Export Hardware
  ☑ Include bitstream
  → 保存为 buck_hil_wrapper.xsa
```

### 8.2 导入 Vitis

```
Vitis → Create Platform Project → 选择 buck_hil_wrapper.xsa
→ 在生成的 BSP 中找到 xparameters.h（含全部寄存器基址）
→ 用 xparameters.h 中的宏覆盖 ps/inc/platform.h 中的 PL_REG_BASE
```

---

## 9. 调试技巧

### 9.1 插入 ILA（集成逻辑分析仪）

在 `buck_hil_top.sv` 中添加 ILA 观察关键信号：

```tcl
# Tcl Console:
create_debug_core u_ila_0 ila
set_property C_DATA_DEPTH 4096 [get_debug_cores u_ila_0]
set_property C_NUM_OF_PROBES 8 [get_debug_cores u_ila_0]
set_property port_width 1  [get_debug_ports u_ila_0/probe0]
# ... 连接需要观察的信号
```

重新综合+实现+生成 Bitstream 后，在 Hardware Manager 中观察波形。

### 9.2 常用 ILA 观察点

| 信号 | 域 | 用途 |
|------|-----|------|
| `pwm_duty_q16_sync` | 100MHz | 确认 CDC 后的占空比正确 |
| `solver_vout[31:16]` | 100MHz | 求解器 Vout 整数部分 |
| `dac_update_strobe` | 100MHz | DAC 更新速率 |
| `trig_occurred` | 100MHz | 触发是否正常 |
| `dac_cs_n` | 100MHz | SPI 帧时序 |
| `pwm_lost` | 400MHz | PWM 丢失告警 |

### 9.3 Verilator 预验证

Bitstream 前先用 Verilator 验证功能正确性：

```bash
cd fpga/tb
verilator --cc ../rtl/buck_solver.sv --exe buck_solver_tb.sv
make -C obj_dir -f Vbuck_solver.mk
./obj_dir/Vbuck_solver
```

---

## 10. 故障排除

| 现象 | 可能原因 | 排查 |
|------|---------|------|
| `ERROR: [DRC NSTD-1] Unspecified I/O Standard` | 引脚未约束 | 确认 xdc 中 `TBD` 已全部替换 |
| `ERROR: [Place 30-574]` | 引脚位置非法 | 检查引脚号对应该 Bank 是否支持 LVCMOS33 |
| 综合后存在 latch | `always_comb` 不完整 | 检查 case 是否覆盖所有分支 |
| 时序违规 WNS < 0 | 400MHz 路径过长 | 降低 clk_400 或优化 pwm_capture |
| Bitstream 加载后无 DAC 输出 | DAC 初始化未执行 | 确认 PS 固件写了 `DAC_CTRL` bit0 |
| TCP 不通 | PS 配置缺少 GEM | 在 Block Design PS 中启用 GEM0 |

---

## 11. 快速参考命令

```tcl
# 完整的命令行流程（无 GUI）
vivado -mode batch -source create_vivado_project.tcl
vivado -mode batch -source run_all.tcl   # 需另外编写

# 仅综合
launch_runs synth_1 -jobs 8
wait_on_run synth_1

# 查看利用率
open_run synth_1
report_utilization

# 仅实现
launch_runs impl_1 -jobs 8
wait_on_run impl_1

# 查看时序
open_run impl_1
report_timing_summary

# 写 bitstream
write_bitstream -force buck_hil_top.bit
```

---

## 版本记录

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-05-09 | v1.0 | 初版：工程创建、引脚分配、综合实现、调试指南 |
