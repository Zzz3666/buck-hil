//=============================================================================
// Module: axi_mm_regs — AXI-Lite 寄存器从模块
//
// 轻量级手写 AXI-Lite slave，将 32-bit 寄存器映射到内部信号。
// 地址空间: 8 bit → 256 字节 = 64 个 32-bit 寄存器
//
// 写通道: 组合地址译码 + 时序数据锁存
// 读通道: 地址译码 → MUX → 寄存器输出
//
// param_update: 写 CTRL 寄存器 bit2 时产生单周期脉冲
//=============================================================================

module axi_mm_regs #(
    parameter int AW = 8,    // address width (bytes)
    parameter int DW = 32    // data width
) (
    input  logic        clk,
    input  logic        rst_n,

    // ---- AXI-Lite Write Address ----
    input  logic [AW-1:0] s_axi_awaddr,
    input  logic          s_axi_awvalid,
    output logic          s_axi_awready,

    // ---- AXI-Lite Write Data ----
    input  logic [DW-1:0] s_axi_wdata,
    input  logic [DW/8-1:0] s_axi_wstrb,
    input  logic          s_axi_wvalid,
    output logic          s_axi_wready,

    // ---- AXI-Lite Write Response ----
    output logic [1:0]    s_axi_bresp,
    output logic          s_axi_bvalid,
    input  logic          s_axi_bready,

    // ---- AXI-Lite Read Address ----
    input  logic [AW-1:0] s_axi_araddr,
    input  logic          s_axi_arvalid,
    output logic          s_axi_arready,

    // ---- AXI-Lite Read Data ----
    output logic [DW-1:0] s_axi_rdata,
    output logic [1:0]    s_axi_rresp,
    output logic          s_axi_rvalid,
    input  logic          s_axi_rready,

    // ---- 寄存器输出 (→ 各功能模块) ----
    output logic [31:0] vin_o,
    output logic [31:0] l_val_o,
    output logic [31:0] c_val_o,
    output logic [31:0] r_load_o,
    output logic [31:0] inv_r_load_o,
    output logic [31:0] r_l_o,
    output logic [31:0] vf_o,
    output logic [31:0] il_max_o,
    output logic [31:0] dac_scale_vout_o,
    output logic [31:0] dac_scale_il_o,
    output logic [31:0] dac_update_div_o,
    output logic [31:0] dac_ctrl_o,
    output logic [31:0] trig_src_o,
    output logic [31:0] trig_level_o,
    output logic        trig_arm_o,
    output logic [31:0] ctrl_o,
    output logic        param_update_o,    // 脉冲

    // ---- 外部输入 (读回) ----
    input  logic        trig_occurred_i,
    input  logic [31:0] status_i,
    input  logic [31:0] dac_vout_i,
    input  logic [31:0] dac_il_i
);

    //=========================================================================
    // 地址别名 (字节偏移 → 寄存器索引)
    //=========================================================================
    localparam logic [7:0] ADDR_VIN            = 8'h00;
    localparam logic [7:0] ADDR_L_VAL          = 8'h04;
    localparam logic [7:0] ADDR_C_VAL          = 8'h08;
    localparam logic [7:0] ADDR_R_LOAD          = 8'h0C;
    localparam logic [7:0] ADDR_INV_R_LOAD     = 8'h10;
    localparam logic [7:0] ADDR_R_L            = 8'h14;
    localparam logic [7:0] ADDR_VF             = 8'h18;
    localparam logic [7:0] ADDR_IL_MAX         = 8'h1C;
    localparam logic [7:0] ADDR_DAC_VOUT       = 8'h20;
    localparam logic [7:0] ADDR_DAC_IL         = 8'h24;
    localparam logic [7:0] ADDR_DAC_SCALE_VOUT  = 8'h28;
    localparam logic [7:0] ADDR_DAC_SCALE_IL   = 8'h2C;
    localparam logic [7:0] ADDR_DAC_UPDATE_DIV  = 8'h30;
    localparam logic [7:0] ADDR_DAC_CTRL       = 8'h34;
    localparam logic [7:0] ADDR_TRIG_SRC       = 8'h40;
    localparam logic [7:0] ADDR_TRIG_LEVEL     = 8'h44;
    localparam logic [7:0] ADDR_TRIG_ARM       = 8'h48;
    localparam logic [7:0] ADDR_TRIG_STATUS    = 8'h4C;
    localparam logic [7:0] ADDR_CTRL           = 8'h80;  // 0x100 >> 2 (word addr)
    localparam logic [7:0] ADDR_STATUS          = 8'h84;  // 0x104 >> 2
    localparam logic [7:0] ADDR_FW_VERSION     = 8'h88;  // 0x108 >> 2
    localparam logic [7:0] ADDR_DEVICE_ID      = 8'h8C;  // 0x10C >> 2

    //=========================================================================
    // 写通道状态机 (简单: AW + W → 锁存 → B)
    //=========================================================================
    typedef enum logic [1:0] {
        W_IDLE, W_DATA, W_RESP
    } write_state_t;

    write_state_t w_state;
    logic [7:0]   w_addr;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            w_state <= W_IDLE;
            w_addr  <= 8'd0;
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b00;
        end else begin
            case (w_state)

            W_IDLE: begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
                s_axi_bvalid  <= 1'b0;
                if (s_axi_awvalid && s_axi_awready) begin
                    w_addr  <= s_axi_awaddr;
                    w_state <= W_DATA;
                end
            end

            W_DATA: begin
                s_axi_awready <= 1'b0;
                if (s_axi_wvalid && s_axi_wready) begin
                    // 锁存写数据到寄存器
                    case (w_addr)
                        ADDR_VIN:           vin_o           <= s_axi_wdata;
                        ADDR_L_VAL:         l_val_o         <= s_axi_wdata;
                        ADDR_C_VAL:         c_val_o         <= s_axi_wdata;
                        ADDR_R_LOAD:         r_load_o         <= s_axi_wdata;
                        ADDR_INV_R_LOAD:    inv_r_load_o    <= s_axi_wdata;
                        ADDR_R_L:           r_l_o           <= s_axi_wdata;
                        ADDR_VF:            vf_o            <= s_axi_wdata;
                        ADDR_IL_MAX:        il_max_o        <= s_axi_wdata;
                        ADDR_DAC_SCALE_VOUT: dac_scale_vout_o <= s_axi_wdata;
                        ADDR_DAC_SCALE_IL:  dac_scale_il_o  <= s_axi_wdata;
                        ADDR_DAC_UPDATE_DIV: dac_update_div_o <= s_axi_wdata;
                        ADDR_DAC_CTRL:      dac_ctrl_o      <= s_axi_wdata;
                        ADDR_TRIG_SRC:      trig_src_o      <= s_axi_wdata;
                        ADDR_TRIG_LEVEL:    trig_level_o    <= s_axi_wdata;
                        ADDR_TRIG_ARM:      trig_arm_o      <= s_axi_wdata[0];
                        ADDR_TRIG_STATUS:   /* W1C */ ;
                        ADDR_CTRL:          ctrl_o          <= s_axi_wdata;
                        default: ;
                    endcase
                    w_state <= W_RESP;
                    s_axi_wready <= 1'b0;
                    s_axi_bvalid <= 1'b1;
                end
            end

            W_RESP: begin
                if (s_axi_bready && s_axi_bvalid) begin
                    s_axi_bvalid <= 1'b0;
                    w_state <= W_IDLE;
                end
            end

            endcase
        end
    end

    //=========================================================================
    // param_update 脉冲生成 (CTRL bit2 写 1 → 脉冲)
    //=========================================================================
    logic ctrl_bit2_d;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            ctrl_bit2_d <= 1'b0;
        else
            ctrl_bit2_d <= ctrl_o[2];
    end
    assign param_update_o = ctrl_o[2] && !ctrl_bit2_d;  // 上升沿检测

    //=========================================================================
    // 读通道状态机
    //=========================================================================
    typedef enum logic [1:0] {
        R_IDLE, R_DATA
    } read_state_t;

    read_state_t r_state;
    logic [7:0]  r_addr;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            r_state <= R_IDLE;
            r_addr  <= 8'd0;
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
            s_axi_rresp   <= 2'b00;
        end else begin
            case (r_state)

            R_IDLE: begin
                s_axi_arready <= 1'b1;
                s_axi_rvalid  <= 1'b0;
                if (s_axi_arvalid && s_axi_arready) begin
                    r_addr  <= s_axi_araddr;
                    r_state <= R_DATA;
                    s_axi_arready <= 1'b0;
                end
            end

            R_DATA: begin
                // 地址译码 → 读取对应寄存器
                case (r_addr)
                    ADDR_VIN:           s_axi_rdata <= vin_o;
                    ADDR_L_VAL:         s_axi_rdata <= l_val_o;
                    ADDR_C_VAL:         s_axi_rdata <= c_val_o;
                    ADDR_R_LOAD:         s_axi_rdata <= r_load_o;
                    ADDR_INV_R_LOAD:    s_axi_rdata <= inv_r_load_o;
                    ADDR_R_L:           s_axi_rdata <= r_l_o;
                    ADDR_VF:            s_axi_rdata <= vf_o;
                    ADDR_IL_MAX:        s_axi_rdata <= il_max_o;
                    ADDR_DAC_VOUT:      s_axi_rdata <= dac_vout_i;
                    ADDR_DAC_IL:        s_axi_rdata <= dac_il_i;
                    ADDR_DAC_SCALE_VOUT: s_axi_rdata <= dac_scale_vout_o;
                    ADDR_DAC_SCALE_IL:  s_axi_rdata <= dac_scale_il_o;
                    ADDR_DAC_UPDATE_DIV: s_axi_rdata <= dac_update_div_o;
                    ADDR_DAC_CTRL:      s_axi_rdata <= dac_ctrl_o;
                    ADDR_TRIG_SRC:      s_axi_rdata <= trig_src_o;
                    ADDR_TRIG_LEVEL:    s_axi_rdata <= trig_level_o;
                    ADDR_TRIG_ARM:      s_axi_rdata <= {31'd0, trig_arm_o};
                    ADDR_TRIG_STATUS:   s_axi_rdata <= {31'd0, trig_occurred_i};
                    ADDR_CTRL:          s_axi_rdata <= ctrl_o;
                    ADDR_STATUS:         s_axi_rdata <= status_i;
                    ADDR_FW_VERSION:    s_axi_rdata <= 32'h0001_0000;  // v1.0.0
                    ADDR_DEVICE_ID:     s_axi_rdata <= 32'hB00B_0001;   // placeholder
                    default:            s_axi_rdata <= 32'hDEAD_BEEF;   // unmapped
                endcase

                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;

                if (s_axi_rready && s_axi_rvalid) begin
                    s_axi_rvalid <= 1'b0;
                    r_state <= R_IDLE;
                end
            end

            endcase
        end
    end

    //=========================================================================
    // 默认值
    //=========================================================================
    // 寄存器在复位后设默认值，由 always_ff 中的 rst_n 分支处理，
    // 此处使用 initial 块或 BSP 初始化顺序保证。
    // 简化: 顶层通过复位和 PS 参数初始化顺序来确保有效值。

endmodule
