#=============================================================================
# create_vivado_project.tcl — Buck HIL Vivado 工程一键创建脚本
#
# 用法 (Vivado Tcl Console):
#   cd <path-to>/buck-hil/fpga/vivado
#   source create_vivado_project.tcl
#
# 或命令行:
#   vivado -mode batch -source create_vivado_project.tcl
#=============================================================================

set project_name    "buck_hil"
set project_dir     "[file dirname [info script]]/build"
set rtl_dir         "[file dirname [info script]]/../rtl"
set constraints_dir "[file dirname [info script]]/../constraints"
set tb_dir          "[file dirname [info script]]/../tb"

# ---- 目标器件 ----
set part_name       "xczu3eg-sfvc784-1-e"
set board_part      ""  ;# 如有开发板, 填 Xilinx 板卡名称

# ---- 清理旧工程 ----
if {[file exists $project_dir]} {
    puts "Removing old project at $project_dir"
    file delete -force $project_dir
}

# ---- 创建工程 ----
puts "Creating project: $project_name"
create_project $project_name $project_dir -part $part_name -force

# ---- 设置语言 ----
set_property target_language Verilog [current_project]
# 注意: 虽然源文件是 .sv 扩展名, Vivado 会自动识别 SystemVerilog

#=============================================================================
# 添加 RTL 源文件
#=============================================================================
puts "Adding RTL sources..."

set rtl_files [list \
    [file join $rtl_dir "buck_hil_top.sv"] \
    [file join $rtl_dir "axi_mm_regs.sv"] \
    [file join $rtl_dir "pwm_capture.sv"] \
    [file join $rtl_dir "buck_solver.sv"] \
    [file join $rtl_dir "dac_interface.sv"] \
    [file join $rtl_dir "capture_manager.sv"] \
]

foreach f $rtl_files {
    if {[file exists $f]} {
        add_files -norecurse $f
        puts "  + [file tail $f]"
    } else {
        puts "  ! MISSING: $f"
    }
}

set_property top buck_hil_top [current_fileset]
add_files -fileset constrs_1 -norecurse [file join $constraints_dir "zu3eg.xdc"]

#=============================================================================
# 添加仿真源文件 (可选)
#=============================================================================
if {[file exists [file join $tb_dir "buck_solver_tb.sv"]]} {
    add_files -fileset sim_1 -norecurse [file join $tb_dir "buck_solver_tb.sv"]
    puts "  + buck_solver_tb.sv (simulation)"
}

#=============================================================================
# 创建 Block Design (ZynqMP PS + AXI Interconnect)
#=============================================================================
puts "Creating Block Design..."

create_bd_design "system"

# ---- IP 版本自动探测 ----
# 策略: 查询 IP catalog → 查不到则用列表第一个版本（该 Vivado 大概率匹配）
proc get_ip_vlnv {ip_name fallback_list} {
    catch { update_ip_catalog -quiet -repo_paths {} }

    set matches {}
    catch { set matches [get_ipdefs -filter "VLNV =~ \"xilinx.com:ip:${ip_name}:*\""] }

    if {[llength $matches] > 0} {
        set vlnv [lindex $matches end]
        puts "    (catalog) $vlnv"
        return $vlnv
    }

    # catalog 未加载 → 用预置版本（大概率正确，错误会在 create_bd_cell 时报具体版本号）
    set vlnv "xilinx.com:ip:${ip_name}:[lindex $fallback_list 0]"
    puts "    (default)  $vlnv"
    return $vlnv
}

# 版本列表: 第一个 = 最可能匹配的默认值
set zynq_vlnv    [get_ip_vlnv "zynq_ultra_ps_e"  {3.5 3.4 3.3 3.2}]
set axi_ic_vlnv   [get_ip_vlnv "axi_interconnect"  {2.1 2.0}]
set axi_dma_vlnv  [get_ip_vlnv "axi_dma"           {7.1 7.0}]
set proc_rst_vlnv [get_ip_vlnv "proc_sys_reset"    {5.0 4.0}]
set util_buf_vlnv [get_ip_vlnv "util_ds_buf"       {2.2 2.1}]
set clk_wiz_vlnv  [get_ip_vlnv "clk_wiz"           {6.0 5.4 5.1}]

puts "  Detected IP versions:"
puts "    ZynqMP PS:      $zynq_vlnv"
puts "    AXI Intercon:   $axi_ic_vlnv"
puts "    AXI DMA:        $axi_dma_vlnv"
puts "    Proc Sys Reset: $proc_rst_vlnv"

# --- 1. Zynq UltraScale+ MPSoC ---
set zynq [create_bd_cell -type ip -vlnv $zynq_vlnv zynq_ultra_ps_e]

# 配置 PS (Vivado 2025.2 apply_bd_automation, 仅移除不可靠的 pl_clk1)
#   - 使能 PL 时钟 0: 100 MHz (solve 域)
#   - 400 MHz capture 域由 MMCM (clk_wiz_0) 从 pl_clk0 倍频生成
#   - 使能 M_AXI_GP0 (AXI-Lite master → PL registers)
#   - 使能 S_AXI_HP0_FPD (AXI slave ← PL DMA)
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e -config {
    apply_board_preset 0
    Master_M_AXI_GP0 1
    Slave_S_AXI_HP0_FPD 1
    pl_clk0 100
    num_fabric_resets 1
} [get_bd_cells $zynq]

# 验证 PS 接口并自动发现 AXI 总线 pin 名
# Vivado 2025.2 命名: M_AXI_HPM0_FPD, S_AXI_HP0_FPD (非 M_AXI_GP0, S_AXI_HP0)
puts "  PS interfaces:"
set zynq_m_axi ""; set zynq_s_axi ""
foreach pin [get_bd_intf_pins -of_objects $zynq] {
    puts "    $pin"
    if {[string match "*M_AXI*" $pin]} { set zynq_m_axi $pin }
    if {[string match "*S_AXI*"  $pin]} { set zynq_s_axi $pin }
}
if {$zynq_m_axi eq ""} { puts "  ERROR: No M_AXI interface found on PS!"; exit 1 }
if {$zynq_s_axi eq ""}  { puts "  WARN:  No S_AXI interface found on PS (DMA may not work)" }

# --- 2. AXI Interconnect (GP0 → PL 寄存器) ---
set axi_intercon [create_bd_cell -type ip -vlnv $axi_ic_vlnv axi_interconnect_0]
set_property -dict [list \
    CONFIG.NUM_MI {1} \
    CONFIG.NUM_SI {1} \
] $axi_intercon

# --- 3. AXI DMA (S2MM channel only, capture → PS DDR) ---
set axi_dma [create_bd_cell -type ip -vlnv $axi_dma_vlnv axi_dma_0]
set_property -dict [list \
    CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_s2mm_burst_size {16} \
    CONFIG.c_m_axi_s2mm_data_width {64} \
] $axi_dma

# --- 4. Processor System Reset (for PL clock domains) ---
set rst_100 [create_bd_cell -type ip -vlnv $proc_rst_vlnv rst_100]
set rst_400 [create_bd_cell -type ip -vlnv $proc_rst_vlnv rst_400]

# --- 5. Utility Buffer (LVDS clock input) ---
set util_buf [create_bd_cell -type ip -vlnv $util_buf_vlnv util_ds_buf_0]

# --- 5b. Clocking Wizard (100MHz → 400MHz for capture domain) ---
#      ZU+ PS pl_clk1 不保证可用, 用 MMCM 从 pl_clk0 倍频
set clk_wiz [create_bd_cell -type ip -vlnv $clk_wiz_vlnv clk_wiz_0]
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ  {100.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {400.000} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_RESET  {false} \
] $clk_wiz

# --- 6. Concat (interrupts) — Vivado 2025.2+ 使用 ilconcat
set concat [create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilconcat:1.0 ilconcat_0]
set_property -dict [list CONFIG.NUM_PORTS {4}] $concat

#=============================================================================
# 连接时钟
#=============================================================================
# PL 时钟来自 PS
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $rst_100/slowest_sync_clk]
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $axi_intercon/ACLK]
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $axi_intercon/M00_ACLK]
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $axi_intercon/S00_ACLK]
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $axi_dma/s_axi_lite_aclk]
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $axi_dma/m_axi_s2mm_aclk]

# MMCM 输入: pl_clk0 (100MHz)
connect_bd_net [get_bd_pins $zynq/pl_clk0] [get_bd_pins $clk_wiz/clk_in1]
# 400MHz capture clock → rst_400
connect_bd_net [get_bd_pins $clk_wiz/clk_out1] [get_bd_pins $rst_400/slowest_sync_clk]

#=============================================================================
# 连接复位
#=============================================================================
connect_bd_net [get_bd_pins $zynq/pl_resetn0] [get_bd_pins $rst_100/ext_reset_in]
connect_bd_net [get_bd_pins $rst_100/peripheral_aresetn] [get_bd_pins $axi_intercon/ARESETN]
connect_bd_net [get_bd_pins $rst_100/peripheral_aresetn] [get_bd_pins $axi_intercon/M00_ARESETN]
connect_bd_net [get_bd_pins $rst_100/peripheral_aresetn] [get_bd_pins $axi_intercon/S00_ARESETN]
connect_bd_net [get_bd_pins $rst_100/peripheral_aresetn] [get_bd_pins $axi_dma/axi_resetn]

connect_bd_net [get_bd_pins $zynq/pl_resetn0] [get_bd_pins $rst_400/ext_reset_in]

#=============================================================================
# 连接 AXI 总线
#=============================================================================
# GP0/HPM0 → Interconnect (使用自动发现的 pin 名)
connect_bd_intf_net [get_bd_intf_pins $zynq_m_axi] [get_bd_intf_pins $axi_intercon/S00_AXI]
# DMA → HP0_FPD (使用自动发现的 pin 名)
if {$zynq_s_axi ne ""} {
    connect_bd_intf_net [get_bd_intf_pins $axi_dma/M_AXI_S2MM] [get_bd_intf_pins $zynq_s_axi]
}

#=============================================================================
# 连接中断
# Vivado 2025.2: ilconcat dout → dout[3:0], PS pl_ps_irq0 → pl_ps_irq[0:0]
# 使用自动发现避免版本间 pin 名变化
#=============================================================================
puts "  Interrupt pins:"

# 发现 concat 的输入/输出 pin
set concat_in0 ""; set concat_dout ""
foreach pin [get_bd_pins -of_objects $concat] {
    puts "    concat: $pin"
    if {[string match "*In0*" $pin]}   { set concat_in0 $pin }
    if {[string match "*dout*" $pin]}  { set concat_dout $pin }
}
if {$concat_in0 eq "" || $concat_dout eq ""} {
    puts "  ERROR: Cannot find concat In0/dout pins!"; exit 1
}

# 发现 PS 中断输入 pin
# Vivado 2025.2: pl_ps_irq 是 bus pin (width 1), -filter {DIR == I} 对其无效
set zynq_irq ""
# 策略 1: 无 DIR 过滤，匹配所有含 "irq" 的 pin（含 bus pin）
foreach pin [get_bd_pins -of_objects $zynq] {
    if {[string match -nocase "*irq*" $pin]} { set zynq_irq $pin; puts "    zynq: $pin" }
}
# 策略 2: 兜底 — 显式 bus 索引（Vivado 2025.2 常见命名）
if {$zynq_irq eq ""} {
    if {[catch { set zynq_irq [get_bd_pins $zynq/pl_ps_irq] } err]} {
        puts "    WARN: pl_ps_irq not found either"
    } else {
        puts "    zynq: $zynq_irq (fallback)"
    }
}
if {$zynq_irq eq ""} {
    puts "  ERROR: No IRQ input pin found on PS! Tried all strategies."
    puts "  Dump all Zynq pins for debugging:"
    foreach pin [get_bd_pins -of_objects $zynq] { puts "    $pin" }
    exit 1
}

# DMA s2mm_introut → concat In0
set dma_intr [get_bd_pins $axi_dma/s2mm_introut]
connect_bd_net [get_bd_pins $dma_intr] [get_bd_pins $concat_in0]
# concat dout → PS IRQ
connect_bd_net [get_bd_pins $concat_dout] [get_bd_pins $zynq_irq]

#=============================================================================
# 创建 HDL Wrapper 并关联顶层
#=============================================================================
# 注意: 由于我们使用的是手写顶层 (buck_hil_top),
# 不需要 Auto-generated HDL wrapper。
# Block Design 仅用于 PS 配置和 AXI 互联。
# buck_hil_top 通过端口连接到 Block Design 的接口。

puts "Block Design created. You need to:"
puts "  1. Connect buck_hil_top ports to Block Design ports in the wrapper"
puts "  2. Or: instantiate the Block Design inside buck_hil_top"

#=============================================================================
# 生成 Block Design 输出产品
#=============================================================================
regenerate_bd_layout
validate_bd_design
save_bd_design

#=============================================================================
# 运行综合 (可选, 取消注释以启用)
#=============================================================================
# launch_runs synth_1 -jobs 8
# wait_on_run synth_1

#=============================================================================
# 运行实现 + 生成 Bitstream (可选)
#=============================================================================
# launch_runs impl_1 -to_step write_bitstream -jobs 8
# wait_on_run impl_1

puts ""
puts "============================================"
puts "  Buck HIL Vivado Project Created"
puts "  Location: $project_dir"
puts "============================================"
puts ""
puts "Next steps:"
puts "  1. Open the project in Vivado GUI:"
puts "     vivado $project_dir/$project_name.xpr"
puts ""
puts "  2. Review Block Design connections"
puts "  3. Connect buck_hil_top ports in wrapper"
puts "  4. Run Synthesis → Implementation → Bitstream"
puts ""
