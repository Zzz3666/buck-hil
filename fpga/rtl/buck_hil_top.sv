//=============================================================================
// Module: buck_hil_top — Buck HIL PL 顶层
//
// 实例化全部子模块，处理跨时钟域和 AXI 接口。
//
// 时钟域:
//   clk_400   — 400 MHz  (pwm_capture)
//   clk_100   — 100 MHz  (solver, dac, regs, capture)
//
// AXI 接口:
//   S_AXI_GP0  — AXI-Lite Slave (PS → PL 寄存器)
//   M_AXIS_DMA — AXI-Stream Master (capture → PS DMA)
//=============================================================================

module buck_hil_top (
    // --- 时钟和复位 ---
    input  logic        clk_400_p,       // 400 MHz LVDS (P side)
    input  logic        clk_400_n,       // 400 MHz LVDS (N side)
    input  logic        clk_100,         // 100 MHz (PL fabric, from PS)
    input  logic        rst_n,           // 全局异步复位, 低有效

    // --- PWM 输入 (来自 DUT 控制器) ---
    input  logic        pwm_in,          // 3.3V LVCMOS

    // --- DAC80508 SPI ---
    output logic        dac_sclk,        // 50 MHz SCLK
    output logic        dac_sdi,         // Serial Data In
    output logic        dac_cs_n,        // Chip Select, 低有效
    output logic        dac_ldac_n,      // LDAC, 接 GND

    // --- AXI-Lite Slave (PS M_AXI_GP0) ---
    input  logic        s_axi_aclk,
    input  logic        s_axi_aresetn,
    input  logic [7:0]  s_axi_awaddr,
    input  logic        s_axi_awvalid,
    output logic        s_axi_awready,
    input  logic [31:0] s_axi_wdata,
    input  logic [3:0]  s_axi_wstrb,
    input  logic        s_axi_wvalid,
    output logic        s_axi_wready,
    output logic [1:0]  s_axi_bresp,
    output logic        s_axi_bvalid,
    input  logic        s_axi_bready,
    input  logic [7:0]  s_axi_araddr,
    input  logic        s_axi_arvalid,
    output logic        s_axi_arready,
    output logic [31:0] s_axi_rdata,
    output logic [1:0]  s_axi_rresp,
    output logic        s_axi_rvalid,
    input  logic        s_axi_rready,

    // --- AXI-Stream Master (捕获数据 → PS DMA) ---
    output logic [63:0] m_axis_tdata,
    output logic        m_axis_tvalid,
    input  logic        m_axis_tready,
    output logic        m_axis_tlast,

    // --- 调试 ---
    output logic [3:0]  debug_led
);

    //=========================================================================
    // clk_400 差分 → 单端
    //=========================================================================
    logic clk_400;
    IBUFGDS #(
        .DIFF_TERM("TRUE"),
        .IBUF_LOW_PWR("FALSE")
    ) ibuf_clk400 (
        .I  (clk_400_p),
        .IB (clk_400_n),
        .O  (clk_400)
    );

    //=========================================================================
    // 复位同步 (异步置位, 同步释放 — 每个时钟域独立)
    //=========================================================================
    logic rst_400_n, rst_100_n;

    // 400 MHz 域复位
    logic [2:0] rst_sync_400;
    always_ff @(posedge clk_400 or negedge rst_n) begin
        if (!rst_n)
            rst_sync_400 <= '0;
        else
            rst_sync_400 <= {rst_sync_400[1:0], 1'b1};
    end
    assign rst_400_n = rst_sync_400[2];

    // 100 MHz 域复位
    logic [2:0] rst_sync_100;
    always_ff @(posedge clk_100 or negedge rst_n) begin
        if (!rst_n)
            rst_sync_100 <= '0;
        else
            rst_sync_100 <= {rst_sync_100[1:0], 1'b1};
    end
    assign rst_100_n = rst_sync_100[2];

    //=========================================================================
    // 1. pwm_capture (400 MHz domain)
    //=========================================================================
    logic [31:0] pwm_duty_q16;
    logic        pwm_duty_valid;
    logic [19:0] pwm_measured_freq;
    logic        pwm_freq_valid;
    logic        pwm_lost;

    pwm_capture #(
        .CLK_FREQ(400_000_000),
        .DEBOUNCE_TICKS(2),
        .CONSISTENCY_CYCLES(3),
        .TIMEOUT_CYCLES(10000)
    ) u_pwm_capture (
        .clk            (clk_400),
        .rst_n          (rst_400_n),
        .pwm_in         (pwm_in),
        .expected_freq  (16'd200),         // 来自 AXI 寄存器, 暂时硬编码
        .duty_q16       (pwm_duty_q16),
        .duty_valid     (pwm_duty_valid),
        .measured_freq  (pwm_measured_freq),
        .freq_valid     (pwm_freq_valid),
        .pwm_lost       (pwm_lost)
    );

    //=========================================================================
    // 2. CDC: 400 MHz → 100 MHz (duty_q16 + duty_valid)
    //
    // 策略: 两拍同步 + 脉冲拉伸
    //   duty_valid 在 400MHz 域中为单周期脉冲，
    //   拉伸到 3 个 100MHz cycle (~30ns) 确保被捕获。
    //=========================================================================
    logic        duty_valid_stretched;
    logic        duty_valid_sync_1, duty_valid_sync_2, duty_valid_sync_3;
    logic [31:0] pwm_duty_q16_sync;
    logic [1:0]  duty_valid_100_stretch;

    // 脉冲拉伸 (400MHz 域): duty_valid → 保持高 5 个 400MHz cycle
    logic [2:0] stretch_cnt_400;
    logic       duty_valid_400_stretched;
    always_ff @(posedge clk_400 or negedge rst_400_n) begin
        if (!rst_400_n) begin
            stretch_cnt_400 <= '0;
            duty_valid_400_stretched <= 1'b0;
        end else begin
            if (pwm_duty_valid) begin
                duty_valid_400_stretched <= 1'b1;
                stretch_cnt_400 <= '0;
            end else if (duty_valid_400_stretched) begin
                if (stretch_cnt_400 >= 3'd4)
                    duty_valid_400_stretched <= 1'b0;
                else
                    stretch_cnt_400 <= stretch_cnt_400 + 3'd1;
            end
        end
    end

    // 跨域同步 (2-flop)
    always_ff @(posedge clk_100 or negedge rst_100_n) begin
        if (!rst_100_n) begin
            duty_valid_sync_1 <= 1'b0;
            duty_valid_sync_2 <= 1'b0;
            duty_valid_sync_3 <= 1'b0;
            pwm_duty_q16_sync <= 32'd0;
            duty_valid_100_stretch <= '0;
        end else begin
            // duty sync
            duty_valid_sync_1 <= duty_valid_400_stretched;
            duty_valid_sync_2 <= duty_valid_sync_1;

            // 边沿检测: 上升沿 → 单周期 pulse
            duty_valid_sync_3 <= duty_valid_sync_2;

            // 在 duty_valid 稳定时锁存 duty_q16
            if (duty_valid_sync_2 && !duty_valid_sync_3)
                pwm_duty_q16_sync <= pwm_duty_q16;
        end
    end
    assign duty_valid_stretched = duty_valid_sync_2 && !duty_valid_sync_3;

    //=========================================================================
    // 3. AXI-Lite 寄存器模块
    //=========================================================================
    logic [31:0] reg_vin, reg_l_val, reg_c_val, reg_r_load;
    logic [31:0] reg_inv_r_load, reg_r_l, reg_vf, reg_il_max;
    logic [31:0] reg_dac_vout, reg_dac_il, reg_dac_scale_vout, reg_dac_scale_il;
    logic [31:0] reg_dac_update_div, reg_dac_ctrl;
    logic [31:0] reg_trig_src, reg_trig_level;
    logic        reg_trig_arm, reg_trig_occurred;
    logic [31:0] reg_ctrl, reg_status;
    logic        reg_param_update;    // 脉冲: 参数更新触发

    axi_mm_regs u_axi_regs (
        .clk            (clk_100),
        .rst_n          (rst_100_n),

        // AXI-Lite
        .s_axi_awaddr   (s_axi_awaddr),
        .s_axi_awvalid  (s_axi_awvalid),
        .s_axi_awready  (s_axi_awready),
        .s_axi_wdata    (s_axi_wdata),
        .s_axi_wstrb    (s_axi_wstrb),
        .s_axi_wvalid   (s_axi_wvalid),
        .s_axi_wready   (s_axi_wready),
        .s_axi_bresp    (s_axi_bresp),
        .s_axi_bvalid   (s_axi_bvalid),
        .s_axi_bready   (s_axi_bready),
        .s_axi_araddr   (s_axi_araddr),
        .s_axi_arvalid  (s_axi_arvalid),
        .s_axi_arready  (s_axi_arready),
        .s_axi_rdata    (s_axi_rdata),
        .s_axi_rresp    (s_axi_rresp),
        .s_axi_rvalid   (s_axi_rvalid),
        .s_axi_rready   (s_axi_rready),

        // 寄存输出
        .vin_o          (reg_vin),
        .l_val_o        (reg_l_val),
        .c_val_o        (reg_c_val),
        .r_load_o       (reg_r_load),
        .inv_r_load_o   (reg_inv_r_load),
        .r_l_o          (reg_r_l),
        .vf_o           (reg_vf),
        .il_max_o       (reg_il_max),
        .dac_scale_vout_o (reg_dac_scale_vout),
        .dac_scale_il_o  (reg_dac_scale_il),
        .dac_update_div_o (reg_dac_update_div),
        .dac_ctrl_o     (reg_dac_ctrl),
        .trig_src_o     (reg_trig_src),
        .trig_level_o   (reg_trig_level),
        .trig_arm_o     (reg_trig_arm),
        .trig_occurred_i(reg_trig_occurred),
        .ctrl_o         (reg_ctrl),
        .status_i       (reg_status),
        .param_update_o (reg_param_update),

        // 读回路径
        .dac_vout_i     (reg_dac_vout),
        .dac_il_i       (reg_dac_il)
    );

    //=========================================================================
    // 4. buck_solver (100 MHz domain)
    //=========================================================================
    logic [31:0] solver_vout, solver_il;
    logic        solver_pwm_active, solver_step_valid;

    buck_solver #(
        .IL_MAX (32'h000A_0000),
        .VIN_MAX(32'h0019_0000),
        .PERIOD (500),
        .STEP_DIV(10)
    ) u_buck_solver (
        .clk            (clk_100),
        .rst_n          (rst_100_n),
        .duty_q16_in    (pwm_duty_q16_sync),
        .duty_valid_in  (duty_valid_stretched),
        .vin            (reg_vin),
        .l_val          (reg_l_val),
        .c_val          (reg_c_val),
        .r_load         (reg_r_load),
        .inv_r_load     (reg_inv_r_load),
        .r_l            (reg_r_l),
        .vf             (reg_vf),
        .param_update   (reg_param_update),
        .v_out          (solver_vout),
        .i_l_out        (solver_il),
        .pwm_active_out (solver_pwm_active),
        .step_valid     (solver_step_valid)
    );

    //=========================================================================
    // 5. DAC 数据转换: Q16.16 → 16-bit DAC code
    //
    // Vout: Q16.16 V → range 0-12V → DAC 0-5V (gain=2) → ×2.4 amp → 0-12V
    //       dac_code = Vout / 12.0 * 65535
    //       scale = 65535/12 ≈ 5461 (整数)
    //
    // IL:   Q16.16 A → range 0-10A → DAC 0-5V → sense resistor mapping
    //       dac_code = IL / 10.0 * 65535
    //       scale = 65535/10 ≈ 6554 (整数)
    //
    // 公式: dac_code = (solver_out[31:16] * scale) → 取高 16 位
    //=========================================================================
    logic [31:0] vout_int, il_int;
    assign vout_int = solver_vout[31:16];  // 整数部分 (0-12)
    assign il_int   = solver_il[31:16];    // 整数部分 (0-10)

    // PS 写入的缩放因子 (Q16.16 格式, 但实际是整数 5461/6554 左移 16)
    // 直接从 scale 的高 16 位取整数部分用于乘法
    logic [15:0] scale_vout_int, scale_il_int;
    assign scale_vout_int = reg_dac_scale_vout[31:16];  // ≈ 5461
    assign scale_il_int   = reg_dac_scale_il[31:16];    // ≈ 6554

    // 16-bit × 16-bit = 32-bit, DAC code = 高 16 位
    logic [31:0] dac_code_vout_full, dac_code_il_full;
    assign dac_code_vout_full = vout_int * scale_vout_int;
    assign dac_code_il_full   = il_int * scale_il_int;

    // DAC80508: 16-bit code (0-65535), 但乘法结果可能需要限制
    wire [15:0] dac_code_vout = (vout_int > 16'd12)
                               ? 16'hFFFF
                               : dac_code_vout_full[15:0];
    wire [15:0] dac_code_il   = (il_int > 16'd10)
                               ? 16'hFFFF
                               : dac_code_il_full[15:0];

    //=========================================================================
    // DAC 更新选通生成
    //
    // solver_step_valid 每 100ns 一次 (10MHz)
    // update_strobe = step_valid && (div_counter == 0)
    // 分频器由 reg_dac_update_div 控制
    //=========================================================================
    logic [9:0] dac_div_counter;
    logic       dac_update_strobe;
    always_ff @(posedge clk_100 or negedge rst_100_n) begin
        if (!rst_100_n) begin
            dac_div_counter <= '0;
            dac_update_strobe <= 1'b0;
        end else begin
            dac_update_strobe <= 1'b0;
            if (solver_step_valid) begin
                if (dac_div_counter >= reg_dac_update_div[9:0]) begin
                    dac_div_counter <= '0;
                    dac_update_strobe <= 1'b1;
                end else begin
                    dac_div_counter <= dac_div_counter + 10'd1;
                end
            end
        end
    end

    // 保存当前 DAC code (用于 axi_mm_regs 读回)
    always_ff @(posedge clk_100 or negedge rst_100_n) begin
        if (!rst_100_n) begin
            reg_dac_vout <= 32'd0;
            reg_dac_il   <= 32'd0;
        end else begin
            if (dac_update_strobe) begin
                reg_dac_vout <= {16'd0, dac_code_vout};
                reg_dac_il   <= {16'd0, dac_code_il};
            end
        end
    end

    //=========================================================================
    // 6. dac_interface (100 MHz domain)
    //=========================================================================
    dac_interface #(
        .SPI_CLK_DIV(1),
        .NUM_CHANNELS(2)
    ) u_dac_interface (
        .clk            (clk_100),
        .rst_n          (rst_100_n),
        .ch0_data       (dac_code_vout),
        .ch1_data       (dac_code_il),
        .ch2_data       (16'd0),
        .ch3_data       (16'd0),
        .ch4_data       (16'd0),
        .ch5_data       (16'd0),
        .ch6_data       (16'd0),
        .ch7_data       (16'd0),
        .update_strobe  (dac_update_strobe),
        .spi_sclk       (dac_sclk),
        .spi_sdi        (dac_sdi),
        .spi_cs_n       (dac_cs_n),
        .spi_ldac_n     (dac_ldac_n)
    );

    //=========================================================================
    // 7. capture_manager (100 MHz domain)
    //=========================================================================
    logic cap_trig_occurred;

    capture_manager #(
        .BUFFER_DEPTH(8192),
        .PRE_TRIGGER(512),
        .CAPTURE_SIZE(4096)
    ) u_capture_manager (
        .clk            (clk_100),
        .rst_n          (rst_100_n),
        .vout           (solver_vout[31:16]),
        .il             (solver_il[31:16]),
        .duty           (pwm_duty_q16_sync[31:16]),
        .step_valid     (solver_step_valid),
        .trig_src       (reg_trig_src[1:0]),
        .trig_level     (reg_trig_level[15:0]),
        .trig_arm       (reg_trig_arm),
        .trig_occurred  (cap_trig_occurred),
        .m_axis_tdata   (m_axis_tdata),
        .m_axis_tvalid  (m_axis_tvalid),
        .m_axis_tready  (m_axis_tready),
        .m_axis_tlast   (m_axis_tlast)
    );

    assign reg_trig_occurred = cap_trig_occurred;

    //=========================================================================
    // 8. 状态寄存器
    //=========================================================================
    // STATUS: bit0=running, bit1=pwm_ok, bit2=triggered
    assign reg_status = {29'd0,
                         cap_trig_occurred,   // bit2
                         ~pwm_lost,           // bit1
                         reg_ctrl[0]};        // bit0

    //=========================================================================
    // 9. 调试 LED
    //=========================================================================
    assign debug_led = {pwm_lost,
                        cap_trig_occurred,
                        solver_step_valid,
                        dac_update_strobe};

endmodule
