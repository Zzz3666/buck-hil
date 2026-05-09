#=============================================================================
# zu3eg.xdc — Buck HIL PL 约束文件 (XCZU3EG-1SFVC784E)
#
# 注: 实际引脚号需根据你的 PCB / 开发板修改。
#     以下为模板，TBD 标记处需填入实际引脚。
#=============================================================================

#=============================================================================
# 时钟约束
#=============================================================================
# PL Fabric Clock 0: 100 MHz (来自 PS pl_clk0)
create_clock -period 10.000 -name clk_100 [get_ports clk_100]

# PL Fabric Clock 1: 400 MHz LVDS (来自 PS pl_clk1)
create_clock -period 2.500 -name clk_400 [get_ports clk_400_p]

# AXI 时钟 (由 PS 生成, 100 MHz)
create_clock -period 10.000 -name s_axi_aclk [get_ports s_axi_aclk]

#=============================================================================
# 时钟组 (异步)
#=============================================================================
set_clock_groups -asynchronous \
    -group [get_clocks clk_100] \
    -group [get_clocks clk_400] \
    -group [get_clocks s_axi_aclk]

#=============================================================================
# PWM 输入 (3.3V LVCMOS, 来自 DUT 控制器)
#=============================================================================
set_property PACKAGE_PIN TBD [get_ports pwm_in]
set_property IOSTANDARD LVCMOS33 [get_ports pwm_in]
set_property SLEW SLOW [get_ports pwm_in]

# 输入延迟 (200kHz PWM, 宽松约束)
set_input_delay -clock [get_clocks clk_400] -max 2.000 [get_ports pwm_in]
set_input_delay -clock [get_clocks clk_400] -min 1.000 [get_ports pwm_in]

#=============================================================================
# DAC80508 SPI (3.3V LVCMOS, 50 MHz)
#=============================================================================
set_property PACKAGE_PIN TBD [get_ports dac_sclk]
set_property PACKAGE_PIN TBD [get_ports dac_sdi]
set_property PACKAGE_PIN TBD [get_ports dac_cs_n]
set_property PACKAGE_PIN TBD [get_ports dac_ldac_n]
set_property IOSTANDARD LVCMOS33 [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]
set_property SLEW FAST [get_ports dac_sclk]

# SPI 输出延迟
set_output_delay -clock [get_clocks clk_100] -max 3.000 \
    [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]
set_output_delay -clock [get_clocks clk_100] -min 1.000 \
    [get_ports {dac_sclk dac_sdi dac_cs_n dac_ldac_n}]

#=============================================================================
# 调试 LED (可选)
#=============================================================================
set_property PACKAGE_PIN TBD [get_ports {debug_led[0]}]
set_property PACKAGE_PIN TBD [get_ports {debug_led[1]}]
set_property PACKAGE_PIN TBD [get_ports {debug_led[2]}]
set_property PACKAGE_PIN TBD [get_ports {debug_led[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {debug_led[*]}]
set_property SLEW SLOW [get_ports {debug_led[*]}]

#=============================================================================
# 时序例外
#=============================================================================
# CDC 同步链 (2-flop) → 放宽时序
# pwm_duty_valid 从 400MHz → 100MHz CDC 路径
set_false_path -from [get_cells u_pwm_capture/*] -to [get_cells *duty_valid_sync*]

# 异步复位
set_false_path -from [get_ports rst_n]

#=============================================================================
# 综合约束
#=============================================================================
# BRAM 推断约束 (capture_manager)
set_property STEPS.SYNTH_DESIGN.ARGS.FLATTEN_HIERARCHY none [get_runs synth_1]

#=============================================================================
# 管脚位置快速参考 (需按实际 PCB 填写)
#
# 信号          | 方向 | IOSTANDARD | 典型 ZU3EG 引脚
# --------------|------|------------|------------------
# clk_400_p     | IN   | LVDS       | (PS pl_clk1 → 全局时钟)
# clk_400_n     | IN   | LVDS       |
# clk_100       | IN   | LVCMOS18   | (PS pl_clk0 → 全局时钟)
# rst_n         | IN   | LVCMOS18   | 按钮/PS复位
# pwm_in        | IN   | LVCMOS33   | PMOD / GPIO
# dac_sclk      | OUT  | LVCMOS33   | PMOD / GPIO
# dac_sdi       | OUT  | LVCMOS33   | PMOD / GPIO
# dac_cs_n      | OUT  | LVCMOS33   | PMOD / GPIO
# dac_ldac_n    | OUT  | LVCMOS33   | PMOD / GPIO
# debug_led[*]  | OUT  | LVCMOS33   | PMOD / GPIO
#=============================================================================
