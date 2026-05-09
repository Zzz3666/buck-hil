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
set part_name       "xczu3eg-sfvc784-1-i"
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

# --- 1. Zynq UltraScale+ MPSoC ---
set zynq [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.4 zynq_ultra_ps_e]

# 配置 PS:
#   - 使能 PL 时钟 0: 100 MHz
#   - 使能 PL 时钟 1: 400 MHz (LVDS)
#   - 使能 M_AXI_GP0 (AXI-Lite master → PL registers)
#   - 使能 S_AXI_HP0 (AXI slave ← PL DMA)
#   - 使能 GEM0 (Ethernet → lwIP)
#   - 使能 UART0 (调试)
#   - 使能 TTC0 (系统定时器)

set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__S_AXI_HP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__FPGA_PL1_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL1_REF_CTRL__FREQMHZ {400} \
    CONFIG.PSU__USE__UART0 {1} \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__S_AXI_HP0 {1} \
    CONFIG.PSU__NUM_FABRIC_RESETS {1} \
] [get_bd_cells $zynq]

# --- 2. AXI Interconnect (GP0 → PL 寄存器) ---
set axi_intercon [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_interconnect_0]
set_property -dict [list \
    CONFIG.NUM_MI {1} \
    CONFIG.NUM_SI {1} \
] $axi_intercon

# --- 3. AXI DMA (S2MM channel only, capture → PS DDR) ---
set axi_dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma_0]
set_property -dict [list \
    CONFIG.c_include_mm2s {0} \
    CONFIG.c_include_s2mm {1} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_s2mm_burst_size {16} \
    CONFIG.c_m_axi_s2mm_data_width {64} \
] $axi_dma

# --- 4. Processor System Reset (for PL clock domains) ---
set rst_100 [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_100]
set rst_400 [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_400]

# --- 5. Utility Buffer (LVDS clock input) ---
set util_buf [create_bd_cell -type ip -vlnv xilinx.com:ip:util_ds_buf:2.2 util_ds_buf_0]

# --- 6. Concat (interrupts) ---
set concat [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_0]
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

connect_bd_net [get_bd_pins $zynq/pl_clk1] [get_bd_pins $rst_400/slowest_sync_clk]

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
# GP0 → Interconnect
connect_bd_intf_net [get_bd_intf_pins $zynq/M_AXI_GP0] [get_bd_intf_pins $axi_intercon/S00_AXI]
# Intercon → PL 寄存器 (在顶层中的 buck_hil_top 处理)
# DMA → HP0
connect_bd_intf_net [get_bd_intf_pins $axi_dma/M_AXI_S2MM] [get_bd_intf_pins $zynq/S_AXI_HP0]

#=============================================================================
# 连接中断
#=============================================================================
connect_bd_net [get_bd_pins $axi_dma/s2mm_introut] [get_bd_pins $concat/In0]
# 其余中断引脚接地
connect_bd_net [get_bd_pins $concat/dout] [get_bd_pins $zynq/pl_ps_irq0]

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
