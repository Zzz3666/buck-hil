//=============================================================================
// Module: capture_manager — 触发数据捕获 + BRAM 缓冲 + AXI-Stream DMA
//
// 功能:
//   1. 每 step_valid 锁存 {vout[15:0], il[15:0], duty[15:0], ts[15:0]} → 64-bit
//   2. 64-bit × BUFFER_DEPTH 环形缓冲区 (BRAM)
//   3. 触发检测: 软件/上升沿/下降沿/电平
//   4. 触发后抓取 CAPTURE_SIZE 点 (含 PRE_TRIGGER 预触发点)
//   5. AXI-Stream 输出 → AXI DMA S2MM → PS DDR
//
// 参数:
//   BUFFER_DEPTH = 8192    (64-bit × 8K = 64 KB BRAM)
//   CAPTURE_SIZE = 4096    (每个触发抓取 4096 点 = 32 KB)
//   PRE_TRIGGER  = 512     (触发前保留 512 点)
//=============================================================================

module capture_manager #(
    parameter int BUFFER_DEPTH = 8192,
    parameter int PRE_TRIGGER  = 512,
    parameter int CAPTURE_SIZE = 4096
) (
    input  logic        clk,
    input  logic        rst_n,

    // ---- 数据输入 ----
    input  logic [15:0] vout,         // Vout, 原始量化值
    input  logic [15:0] il,           // IL
    input  logic [15:0] duty,         // duty cycle
    input  logic        step_valid,    // 每求解步一次

    // ---- 触发配置 ----
    input  logic [1:0]  trig_src,     // 0=软件 1=Vout↑ 2=Vout↓ 3=电平
    input  logic [15:0] trig_level,   // 触发阈值
    input  logic        trig_arm,      // 触发使能

    // ---- 触发状态 ----
    output logic        trig_occurred,

    // ---- AXI-Stream 输出 (到 DMA) ----
    output logic [63:0] m_axis_tdata,
    output logic        m_axis_tvalid,
    input  logic        m_axis_tready,
    output logic        m_axis_tlast
);

    //=========================================================================
    // 时间戳计数器 (自由运行, 16-bit)
    //=========================================================================
    logic [15:0] timestamp;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            timestamp <= 16'd0;
        else if (step_valid)
            timestamp <= timestamp + 16'd1;
    end

    //=========================================================================
    // 环形缓冲区 (BRAM inferred)
    //=========================================================================
    (* ram_style = "block" *)
    logic [63:0] buffer [BUFFER_DEPTH];

    // 写指针 (自由运行, 环形)
    logic [12:0] write_ptr;  // up to 8191
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            write_ptr <= 13'd0;
        else if (step_valid) begin
            buffer[write_ptr] <= {vout, il, duty, timestamp};
            if (write_ptr >= (BUFFER_DEPTH - 1))
                write_ptr <= 13'd0;
            else
                write_ptr <= write_ptr + 13'd1;
        end
    end

    //=========================================================================
    // 触发检测
    //=========================================================================
    typedef enum logic [1:0] {
        T_IDLE, T_ARMED, T_CAPTURING, T_DONE
    } trig_state_t;

    trig_state_t trig_state;
    logic [12:0] trig_write_ptr;   // 触发发生时的写指针位置
    logic [12:0] cap_count;        // 已捕获点数
    logic        prev_vout_msb;    // Vout 前一值 (用于边沿检测)

    // Vout 边沿检测
    logic vout_rising, vout_falling;
    assign vout_rising  = (vout[15] && !prev_vout_msb);     // 简化为 bit15 跳变
    assign vout_falling = (!vout[15] && prev_vout_msb);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            trig_state    <= T_IDLE;
            trig_occurred <= 1'b0;
            trig_write_ptr <= 13'd0;
            cap_count     <= 13'd0;
            prev_vout_msb <= 1'b0;
        end else begin
            // 锁存前一采样
            if (step_valid)
                prev_vout_msb <= vout[15];

            case (trig_state)

            T_IDLE: begin
                trig_occurred <= 1'b0;
                if (trig_arm)
                    trig_state <= T_ARMED;
            end

            T_ARMED: begin
                if (!trig_arm) begin
                    trig_state <= T_IDLE;
                end else begin
                    logic trig_hit;
                    trig_hit = 1'b0;
                    case (trig_src)
                        2'd0: trig_hit = 1'b0;  // 软件触发: 等外部命令
                        2'd1: trig_hit = vout_rising && step_valid;
                        2'd2: trig_hit = vout_falling && step_valid;
                        2'd3: trig_hit = (vout > trig_level) && step_valid;
                        default: ;
                    endcase

                    if (trig_hit && step_valid) begin
                        trig_state     <= T_CAPTURING;
                        trig_occurred  <= 1'b1;
                        trig_write_ptr <= write_ptr;
                        cap_count      <= 13'd0;
                    end
                end
            end

            T_CAPTURING: begin
                if (step_valid) begin
                    if (cap_count >= (CAPTURE_SIZE - 1)) begin
                        trig_state    <= T_DONE;
                        trig_occurred <= 1'b0;
                    end else begin
                        cap_count <= cap_count + 13'd1;
                    end
                end
            end

            T_DONE: begin
                trig_occurred <= 1'b0;
                if (!trig_arm)
                    trig_state <= T_IDLE;
                // 重新 arm 后进入 ARMED 状态等待下次触发
                else if (trig_arm)
                    trig_state <= T_ARMED;
            end

            endcase
        end
    end

    //=========================================================================
    // AXI-Stream 输出 (从 BRAM 读取 → DMA)
    //
    // 触发发生后, 从 trig_write_ptr - PRE_TRIGGER 开始,
    // 顺序读出 CAPTURE_SIZE 点。
    //=========================================================================
    typedef enum logic [1:0] {
        S_IDLE, S_SEND, S_WAIT
    } stream_state_t;

    stream_state_t stream_state;
    logic [12:0]   read_ptr;
    logic [12:0]   send_count;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            stream_state <= S_IDLE;
            read_ptr     <= 13'd0;
            send_count   <= 13'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tdata  <= 64'd0;
        end else begin
            case (stream_state)

            S_IDLE: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                // 检测触发 → 开始发送
                if (trig_state == T_CAPTURING && cap_count == 13'd0) begin
                    // 计算预触发起始地址: trig_write_ptr - PRE_TRIGGER
                    // 注意环形回绕
                    if (trig_write_ptr >= PRE_TRIGGER)
                        read_ptr <= trig_write_ptr - PRE_TRIGGER[12:0];
                    else
                        read_ptr <= BUFFER_DEPTH[12:0] - (PRE_TRIGGER[12:0] - trig_write_ptr);
                    send_count <= 13'd0;
                    stream_state <= S_SEND;
                end
            end

            S_SEND: begin
                m_axis_tvalid <= 1'b1;
                m_axis_tdata  <= buffer[read_ptr];

                if (send_count >= (CAPTURE_SIZE - 1))
                    m_axis_tlast <= 1'b1;
                else
                    m_axis_tlast <= 1'b0;

                if (m_axis_tready && m_axis_tvalid) begin
                    // 推进读指针 (环形)
                    if (read_ptr >= (BUFFER_DEPTH - 1))
                        read_ptr <= 13'd0;
                    else
                        read_ptr <= read_ptr + 13'd1;

                    if (m_axis_tlast) begin
                        stream_state <= S_WAIT;
                        m_axis_tvalid <= 1'b0;
                    end else begin
                        send_count <= send_count + 13'd1;
                    end
                end
            end

            S_WAIT: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                // 等待下一个触发
                if (trig_state == T_IDLE)
                    stream_state <= S_IDLE;
            end

            endcase
        end
    end

endmodule
