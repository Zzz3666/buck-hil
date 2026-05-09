//=============================================================================
// Module: buck_solver
// Purpose: Buck converter switched-state solver — 4-stage pipeline, Q16.16
//=============================================================================
// Clock:      100 MHz (10 ns period) = solver step Δt
// Pipeline:   4 stages, throughput = 1 result/cycle (10 ns)
// Latency:    4 cycles (40 ns)
// Numerics:   Forward Euler, Q16.16 state, Q8.24 coefficients
// Clamping:   i_L ∈ [0, IL_MAX], v_C ∈ [0, VIN_MAX]
//
// Stage 0: Snapshot inputs + decide pwm_active from internal counter
// Stage 1: Compute delta_V terms (both ON and OFF paths in parallel)
// Stage 2: Multiply by dt/L and dt/C → di, dv (select by pwm_active)
// Stage 3: Add to state + clamp + register outputs
//=============================================================================

module buck_solver #(
    parameter int IL_MAX  = 32'h000A_0000,  // 10.0 A  in Q16.16
    parameter int VIN_MAX = 32'h0019_0000,  // 25.0 V  in Q16.16
    parameter int PERIOD  = 500,             // 100 MHz / 200 kHz = 500 cycles (32-bit to match counter width)
    parameter int STEP_DIV = 10             // solver step = clk / STEP_DIV (100MHz/10 = 100ns steps)
) (
    // Clock and reset
    input  logic        clk,           // 100 MHz
    input  logic        rst_n,         // async, active low

    // PWM input from pwm_capture (clk_pl_100 domain)
    input  logic [31:0] duty_q16_in,   // duty cycle Q16.16 [0.0, 1.0]
    input  logic        duty_valid_in,  // single-cycle pulse on duty update

    // Parameters — pre-computed by PS, written via AXI-Lite
    // All parameters are double-buffered externally (param_update triggers snapshot)
    input  logic [31:0] vin,           // Input voltage, Q16.16 (V)
    input  logic [31:0] l_val,         // dt/L, Q8.24 (A/V)
    input  logic [31:0] c_val,         // dt/C, Q8.24 (V/A)
    /* verilator lint_off UNUSED */
    input  logic [31:0] r_load,        // Load resistance, Q16.16 (Ω) — reserved, use inv_r_load instead
    input  logic [31:0] inv_r_load,    // 1/R_load, Q16.16 (1/Ω) — PS pre-computed
    input  logic [31:0] r_l,           // Inductor ESR, Q8.24 (Ω)
    input  logic [31:0] vf,            // Diode forward voltage, Q16.16 (V)
    input  logic        param_update,   // single-cycle pulse, aligned to PWM boundary

    // Outputs
    output logic [31:0] v_out,         // Vout, Q16.16 (V)
    output logic [31:0] i_l_out,       // Inductor current, Q16.16 (A)
    output logic        pwm_active_out, // Current switch state (1 = ON)
    output logic        step_valid      // single-cycle pulse every solver step
);

    //=========================================================================
    // Internal state registers (Q16.16)
    //=========================================================================
    logic [31:0] i_l_reg;    // inductor current
    logic [31:0] v_c_reg;    // capacitor voltage (= Vout)

    //=========================================================================
    // Solver step gating: solver runs at clk / STEP_DIV
    // 100 MHz / 10 = 10 MHz → 100 ns per step
    //=========================================================================
    logic [3:0]  step_counter;
    logic        solve_tick;

    //=========================================================================
    // Parameter snapshot registers (updated on param_update pulse)
    //=========================================================================
    logic [31:0] vin_s;
    logic [31:0] l_val_s;
    logic [31:0] c_val_s;

    logic [31:0] inv_r_load_s;
    logic [31:0] r_l_s;
    logic [31:0] vf_s;

    //=========================================================================
    // Duty cycle register (updated on duty_valid_in pulse)
    //=========================================================================
    logic [31:0] duty_q16_r;

    //=========================================================================
    // Internal PWM generation
    //=========================================================================
    logic [15:0] pwm_counter;
    logic        pwm_active;

    //=========================================================================
    // Pipeline registers
    //=========================================================================

    // ---- Stage 1 registers ----
    logic        s1_valid;
    logic        s1_pwm_active;
    logic [31:0] s1_i_L;
    logic [31:0] s1_v_C;
    logic [31:0] s1_vin;
    logic [31:0] s1_l_val;
    logic [31:0] s1_c_val;
    logic [31:0] s1_inv_r_load;
    logic [31:0] s1_r_l;
    logic [31:0] s1_vf;

    // ---- Stage 2 registers ----
    logic        s2_valid;
    logic        s2_pwm_active;
    logic [31:0] s2_i_L;
    logic [31:0] s2_v_C;
    logic [31:0] s2_l_val;
    logic [31:0] s2_c_val;
    // delta_V terms computed in S1, consumed in S2
    logic [31:0] s2_v_term_on;   // Vin - Vout - i_L*R_L  (Q16.16)
    logic [31:0] s2_v_term_off;  // -Vf - Vout - i_L*R_L   (Q16.16)
    logic [31:0] s2_i_load;      // Vout / R_load = Vout * inv_r_load (Q16.16)

    // ---- Stage 3 registers ----
    logic        s3_valid;
    logic [31:0] s3_i_L_old;
    logic [31:0] s3_v_C_old;
    // di, dv computed in S2, consumed in S3
    logic [31:0] s3_di;  // Q16.16
    logic [31:0] s3_dv;  // Q16.16

    //=========================================================================
    // Initialization, parameter snapshot, duty update — single always_ff
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            i_l_reg      <= 32'h0000_0000;
            v_c_reg      <= 32'h0000_0000;
            duty_q16_r   <= 32'h0000_0000;
            pwm_counter  <= 16'd0;
            step_counter <= 4'd0;
            vin_s        <= 32'h000C_0000;   // default 12V
            l_val_s      <= 32'h0000_0000;
            c_val_s      <= 32'h0000_0000;
            inv_r_load_s <= 32'h0000_1999;   // default 1/10 = 0.1
            r_l_s        <= 32'h0000_0000;
            vf_s         <= 32'h0000_CCCC;   // default ~0.8V
        end else begin
            // Parameter snapshot (double-buffer handoff on param_update pulse)
            if (param_update) begin
                vin_s        <= vin;
                l_val_s      <= l_val;
                c_val_s      <= c_val;
                inv_r_load_s <= inv_r_load;
                r_l_s        <= r_l;
                vf_s         <= vf;
            end

            // Duty cycle update
            if (duty_valid_in)
                duty_q16_r <= duty_q16_in;

            // PWM counter
            if (pwm_counter >= 16'(PERIOD - 1))
                pwm_counter <= 16'd0;
            else
                pwm_counter <= pwm_counter + 16'd1;

            // Solver step counter: generate solve_tick every STEP_DIV cycles
            if (step_counter >= 4'(STEP_DIV - 1))
                step_counter <= 4'd0;
            else
                step_counter <= step_counter + 4'd1;
        end
    end

    assign solve_tick = (step_counter == 4'd0);

    //=========================================================================
    // Internal PWM signal generation
    // pwm_counter: free-running 0..PERIOD-1, resets at period boundary
    // pwm_active:  1 when counter < duty_threshold (SW=ON), else 0 (SW=OFF)
    //
    // duty_threshold = (duty_q16 * PERIOD) >> 16
    // Example: duty=0.5 → 0x8000 * 500 >> 16 = 250 → pwm_active for 250 of 500 cycles
    //=========================================================================
    /* verilator lint_off UNUSED */  // duty_product[15:0] intentionally unused
    wire [31:0] duty_product   = duty_q16_r * PERIOD;
    wire [15:0] duty_threshold = duty_product[31:16];  // integer part

    assign pwm_active = (pwm_counter < duty_threshold);

    //=========================================================================
    // STAGE 0 → STAGE 1: Snapshot inputs
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1_valid       <= 1'b0;
            s1_pwm_active  <= 1'b0;
            s1_i_L         <= 32'd0;
            s1_v_C         <= 32'd0;
            s1_vin         <= 32'd0;
            s1_l_val       <= 32'd0;
            s1_c_val       <= 32'd0;
            s1_inv_r_load  <= 32'd0;
            s1_r_l         <= 32'd0;
            s1_vf          <= 32'd0;
        end else begin
            s1_valid       <= solve_tick; // solver only steps every STEP_DIV cycles
            s1_pwm_active  <= pwm_active;
            s1_i_L         <= i_l_reg;
            s1_v_C         <= v_c_reg;
            s1_vin         <= vin_s;
            s1_l_val       <= l_val_s;
            s1_c_val       <= c_val_s;
            s1_inv_r_load  <= inv_r_load_s;
            s1_r_l         <= r_l_s;
            s1_vf          <= vf_s;
        end
    end

    //=========================================================================
    // STAGE 1 → STAGE 2: Compute intermediate terms
    //
    //   i_L * R_L:   Q16.16 × Q8.24 = Q24.40 → keep Q16.16 by shifting right 24
    //   Vout * inv_R_load: Q16.16 × Q16.16 = Q32.32 → keep Q16.16 by shifting right 16
    //
    //   SW=ON:  v_term = Vin - Vout - i_L*R_L
    //   SW=OFF: v_term = -Vf - Vout - i_L*R_L
    //=========================================================================
    /* verilator lint_off UNUSED */
    // Upper/lower bits of 64-bit products + duty_product[15:0] intentionally unused
    // — we only extract the middle 32 bits after fixed-point alignment.

    logic [63:0] iL_rL_product;  // Q16.16 * Q8.24 = Q24.40
    logic [31:0] iL_rL_drop;     // Q16.16 after shift
    logic [63:0] vC_invR_product; // Q16.16 * Q16.16 = Q32.32
    logic [31:0] i_load;         // Q16.16 after shift

    assign iL_rL_product  = $signed(s1_i_L) * $signed(s1_r_l);
    assign iL_rL_drop     = iL_rL_product[55:24];  // Q24.40 → Q16.16: drop 24 LSBs

    assign vC_invR_product = s1_v_C * s1_inv_r_load;
    assign i_load          = vC_invR_product[47:16];  // Q32.32 → Q16.16: drop 16 LSBs

    // Voltage delta terms (Q16.16, signed)
    wire signed [31:0] v_term_on;
    wire signed [31:0] v_term_off;

    assign v_term_on  = $signed(s1_vin) - $signed(s1_v_C) - $signed(iL_rL_drop);
    assign v_term_off = -$signed(s1_vf)  - $signed(s1_v_C) - $signed(iL_rL_drop);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s2_valid      <= 1'b0;
            s2_pwm_active <= 1'b0;
            s2_i_L        <= 32'd0;
            s2_v_C        <= 32'd0;
            s2_l_val      <= 32'd0;
            s2_c_val      <= 32'd0;
            s2_v_term_on  <= 32'd0;
            s2_v_term_off <= 32'd0;
            s2_i_load     <= 32'd0;
        end else begin
            s2_valid      <= s1_valid;
            s2_pwm_active <= s1_pwm_active;
            s2_i_L        <= s1_i_L;
            s2_v_C        <= s1_v_C;
            s2_l_val      <= s1_l_val;
            s2_c_val      <= s1_c_val;
            s2_v_term_on  <= v_term_on;
            s2_v_term_off <= v_term_off;
            s2_i_load     <= i_load;
        end
    end

    //=========================================================================
    // STAGE 2 → STAGE 3: Multiply by dt/L and dt/C
    //
    //   di = dt/L * v_term     (Q8.24 × Q16.16 = Q24.40 → Q16.16: drop 24 LSBs)
    //   dv = dt/C * (i_L - i_load)   same alignment
    //
    //   Select SW=ON or SW=OFF path based on s2_pwm_active
    //=========================================================================
    logic [63:0] di_product_on;
    logic [63:0] di_product_off;
    logic [31:0] di_on;
    logic [31:0] di_off;
    logic [31:0] di_selected;

    logic [63:0] dv_product;
    logic [31:0] dv_raw;

    wire signed [31:0] i_net = $signed(s2_i_L) - $signed(s2_i_load);

    assign di_product_on  = $signed(s2_l_val) * $signed(s2_v_term_on);
    assign di_product_off = $signed(s2_l_val) * $signed(s2_v_term_off);
    assign dv_product     = $signed(s2_c_val) * i_net;

    assign di_on  = di_product_on[55:24];
    assign di_off = di_product_off[55:24];

    assign di_selected = s2_pwm_active ? di_on : di_off;
    assign dv_raw      = dv_product[55:24];

    /* verilator lint_on UNUSED */

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s3_valid    <= 1'b0;
            s3_i_L_old  <= 32'd0;
            s3_v_C_old  <= 32'd0;
            s3_di       <= 32'd0;
            s3_dv       <= 32'd0;
        end else begin
            s3_valid    <= s2_valid;
            s3_i_L_old  <= s2_i_L;
            s3_v_C_old  <= s2_v_C;
            s3_di       <= di_selected;
            s3_dv       <= dv_raw;
        end
    end

    //=========================================================================
    // STAGE 3 → OUTPUT: Add to state + clamp + register outputs
    //=========================================================================
    logic [31:0] i_l_next;
    logic [31:0] v_c_next;
    logic        i_l_clamped;
    logic        v_c_clamped;

    wire signed [31:0] i_l_pre_clamp = $signed(s3_i_L_old) + $signed(s3_di);
    wire signed [31:0] v_c_pre_clamp = $signed(s3_v_C_old) + $signed(s3_dv);

    // i_L clamp: must be ≥ 0 (diode prevents reverse current)
    //             must be ≤ IL_MAX (saturation)
    // v_C clamp: must be ≥ 0
    //             must be ≤ vin_s (buck cannot boost)
    assign i_l_next = (i_l_pre_clamp[31])
                    ? 32'h0000_0000
                    : ($signed(i_l_pre_clamp) > $signed(IL_MAX))
                        ? IL_MAX
                        : i_l_pre_clamp;

    assign v_c_next = (v_c_pre_clamp[31])
                    ? 32'h0000_0000
                    : ($signed(v_c_pre_clamp) > $signed(vin_s))
                        ? vin_s
                        : v_c_pre_clamp;

    assign i_l_clamped = (i_l_pre_clamp[31]) || ($signed(i_l_pre_clamp) > $signed(IL_MAX));
    assign v_c_clamped = (v_c_pre_clamp[31]) || ($signed(v_c_pre_clamp) > $signed(vin_s));

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            i_l_reg       <= 32'h0000_0000;
            v_c_reg       <= 32'h0000_0000;
            v_out         <= 32'h0000_0000;
            i_l_out       <= 32'h0000_0000;
            pwm_active_out<= 1'b0;
            step_valid    <= 1'b0;
        end else begin
            // Update state registers and outputs on solver step
            if (s3_valid) begin
                i_l_reg <= i_l_next;
                v_c_reg <= v_c_next;
                v_out   <= v_c_next;   // immediate output of new state (not delayed)
                i_l_out <= i_l_next;
            end

            pwm_active_out <= pwm_active;
            step_valid     <= s3_valid;  // aligned with v_out/i_l_out update
        end
    end

    //=========================================================================
    // Simulation-only assertions (ignored by synthesis)
    //=========================================================================
`ifdef VERILATOR
    /* verilator lint_off SYNCASYNCNET */
    // Check for solver divergence (simulation-only, no synthesis)
    always @(posedge clk) begin
        if (rst_n && i_l_clamped && i_l_reg != 32'd0)
            $display("[WARN] buck_solver: i_L clamped at t=%0t (val=0x%h)", $time, i_l_pre_clamp);
        if (rst_n && v_c_clamped && v_c_reg != 32'd0)
            $display("[WARN] buck_solver: v_C clamped at t=%0t (val=0x%h)", $time, v_c_pre_clamp);
    end
`endif

endmodule
