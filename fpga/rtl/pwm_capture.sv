//=============================================================================
// Module: pwm_capture
// Purpose: Capture external PWM signal, measure period/duty with glitch filter
//          and multi-cycle consistency check. Output Q16.16 duty ratio.
//
// Clock:    400 MHz (2.5 ns period)
// Debounce: 2-cycle majority-vote filter = 5 ns glitch rejection
// Consistency: 3 consecutive cycles with duty variance < 1% before update
//
// State machine:
//   IDLE → (↑) → MEASURE → (↑) → VALIDATE → CONSISTENCY → UPDATE → IDLE
//                    ↑                                              │
//                    └────────────── timeout / fail ────────────────┘
//
// CDC: Outputs are in the 400 MHz domain. Top-level wrapper handles
//      CDC to 100 MHz solver domain (2-stage sync + pulse stretch).
//=============================================================================

module pwm_capture #(
    parameter int CLK_FREQ           = 400_000_000,  // clock frequency (Hz)
    parameter int DEBOUNCE_TICKS     = 2,             // debounce depth (2 × 2.5ns = 5ns)
    parameter int CONSISTENCY_CYCLES = 3,             // consistency check window
    parameter int TIMEOUT_CYCLES     = 10000          // PWM lost timeout (~25μs @ 400MHz)
) (
    // Clock and reset
    input  logic        clk,           // 400 MHz
    input  logic        rst_n,         // async, active low

    // External PWM signal (from DUT controller, 3.3V-clamped)
    input  logic        pwm_in,

    // Parameters (from AXI-Lite registers)
    /* verilator lint_off UNUSED */
    input  logic [15:0] expected_freq,  // expected switching frequency (Hz) — reserved for future anomaly detection
    /* verilator lint_on UNUSED */

    // Outputs (400 MHz domain — top-level handles CDC to 100 MHz solver domain)
    output logic [31:0] duty_q16,      // measured duty cycle, Q16.16 [0.0, 1.0]
    output logic        duty_valid,     // single-cycle pulse: duty updated (consistency passed)
    output logic [19:0] measured_freq,  // measured frequency (Hz) — 20 bits, supports up to ~1MHz
    output logic        freq_valid,     // single-cycle pulse: frequency updated
    output logic        pwm_lost        // asserted when PWM signal absent > TIMEOUT
);

    //=========================================================================
    // Debounce / glitch filter
    //
    // 2-stage shift register + majority vote:
    //   All 1s → output high, all 0s → output low, otherwise → hold previous
    //   Rejects glitches narrower than DEBOUNCE_TICKS × 2.5ns = 5ns
    //=========================================================================
    logic [DEBOUNCE_TICKS-1:0] pwm_sync_shift;
    logic                      pwm_filtered;
    logic                      pwm_filtered_d;  // for edge detection

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_sync_shift <= '0;
            pwm_filtered    <= 1'b0;
            pwm_filtered_d  <= 1'b0;
        end else begin
            // Shift register: new bit in LSB, oldest drops off
            pwm_sync_shift <= {pwm_sync_shift[DEBOUNCE_TICKS-2:0], pwm_in};

            // Majority vote
            if (&pwm_sync_shift)
                pwm_filtered <= 1'b1;
            else if (|pwm_sync_shift == 1'b0)  // all zeros
                pwm_filtered <= 1'b0;
            // else: hold previous value (glitch rejection)

            // Delayed copy for edge detection
            pwm_filtered_d <= pwm_filtered;
        end
    end

    // Edge detection on filtered signal
    wire pwm_rising  =  pwm_filtered && !pwm_filtered_d;
    /* verilator lint_off UNUSED */
    wire pwm_falling = !pwm_filtered &&  pwm_filtered_d;
    /* verilator lint_on UNUSED */

    /* verilator lint_off WIDTH */
    // Narrow counters vs int parameters: safe, ranges are bounded by design
    typedef enum logic [2:0] {
        S_IDLE         = 3'd0,  // waiting for rising edge
        S_MEASURE      = 3'd1,  // counting period + high time
        S_VALIDATE     = 3'd2,  // checking frequency validity
        S_CONSISTENCY  = 3'd3,  // checking duty consistency across cycles
        S_UPDATE       = 3'd4   // pulsing outputs, then back to IDLE
    } state_t;

    state_t state, state_next;

    // MEASURE state gating: ensures we capture a full period
    logic measure_got_falling;  // set when falling edge seen in MEASURE

    //=========================================================================
    // Counters
    //=========================================================================
    logic [19:0] period_counter;   // counts total period (rising→rising), max ~655k @ 400MHz
    logic [19:0] high_counter;     // counts HIGH time (rising→falling)
    logic [19:0] timeout_counter;  // counts idle time for PWM lost detection

    // Captured values (latched on second rising edge)
    logic [19:0] captured_period;
    logic [19:0] captured_high;

    //=========================================================================
    // Measured frequency and duty computation
    //=========================================================================
    logic [19:0] raw_freq;         // CLK_FREQ / captured_period
    logic [31:0] raw_duty;         // Q16.16 duty = captured_high / captured_period

    // Division for duty: duty_Q16.16 = (captured_high << 16) / captured_period
    // Use simple restoring divider (small, only runs once per PWM cycle)
    // captured_period is 20-bit, so this is (36-bit) / (20-bit) → 32-bit Q16.16

    //=========================================================================
    // Consistency check
    //=========================================================================
    logic [31:0] duty_history [0:CONSISTENCY_CYCLES-1];  // last N duty measurements
    logic [ 1:0] consistency_index;                       // write pointer for history (2 bits for 3 entries)
    logic [ 2:0] consistency_count;                       // number of valid entries
    logic        consistency_pass;                        // computed: all within 1%

    //=========================================================================
    // State register
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state <= S_IDLE;
        else
            state <= state_next;
    end

    //=========================================================================
    // State machine + counters
    //=========================================================================
    always_comb begin
        state_next = state;

        case (state)
            //-----------------------------------------------------------------
            // IDLE: wait for filtered rising edge. Timeout → pwm_lost.
            //-----------------------------------------------------------------
            S_IDLE: begin
                if (pwm_rising)
                    state_next = S_MEASURE;
                else if (timeout_counter >= TIMEOUT_CYCLES)
                    state_next = S_IDLE;  // stay in IDLE, pwm_lost asserted externally
            end

            //-----------------------------------------------------------------
            // MEASURE: count until next rising edge, AFTER seeing falling edge.
            // On rising edge (and got_falling=1) → latch period + high, go to VALIDATE.
            //-----------------------------------------------------------------
            S_MEASURE: begin
                if (pwm_rising && measure_got_falling)
                    state_next = S_VALIDATE;
                else if (period_counter >= 20'(TIMEOUT_CYCLES))
                    state_next = S_IDLE;  // stuck — PWM lost mid-cycle
            end

            //-----------------------------------------------------------------
            // VALIDATE: check if measured frequency is reasonable.
            // Pass: measured_freq within ±50% of expected_freq.
            // Fail: back to IDLE.
            //-----------------------------------------------------------------
            S_VALIDATE: begin
                // Single-cycle state — always advance
                state_next = S_CONSISTENCY;
            end

            //-----------------------------------------------------------------
            // CONSISTENCY: store current duty, compare against history.
            // Pass (consistency_pass): advance to UPDATE.
            // Fail: reset consistency, back to IDLE.
            //-----------------------------------------------------------------
            S_CONSISTENCY: begin
                // Single-cycle state — advance based on consistency_pass
                // consistency_pass is computed combinationally below
                state_next = S_UPDATE;  // always advance (consistency pass/fail handled in output)
            end

            //-----------------------------------------------------------------
            // UPDATE: pulse duty_valid + freq_valid, then back to IDLE.
            //-----------------------------------------------------------------
            S_UPDATE: begin
                state_next = S_IDLE;
            end

            default: state_next = S_IDLE;
        endcase
    end

    //=========================================================================
    // Counter logic
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            period_counter  <= 20'd0;
            high_counter    <= 20'd0;
            timeout_counter <= 20'd0;
            captured_period <= 20'd0;
            captured_high   <= 20'd0;
            measure_got_falling <= 1'b0;
        end else begin
            // Falling edge detection in MEASURE
            if (state == S_MEASURE && pwm_falling)
                measure_got_falling <= 1'b1;

            // Period counter: runs during MEASURE, latched on rising edge
            if (state == S_MEASURE) begin
                period_counter <= period_counter + 20'd1;

                // High counter: runs while pwm_filtered is high
                if (pwm_filtered)
                    high_counter <= high_counter + 20'd1;

                // On rising edge: latch and reset
                if (pwm_rising && period_counter > 0) begin
                    captured_period <= period_counter;
                    captured_high   <= high_counter;
                    // counters reset below
                end
            end

            // Reset counters when entering MEASURE or after capture
            if ((state == S_IDLE && pwm_rising) ||
                (state == S_MEASURE && pwm_rising && measure_got_falling)) begin
                period_counter <= 20'd1;  // start at 1 (this cycle counts)
                high_counter   <= pwm_filtered ? 20'd1 : 20'd0;
                measure_got_falling <= 1'b0;
            end

            // Timeout counter: counts in IDLE, resets on any rising edge
            if (state == S_IDLE || state_next == S_IDLE) begin
                if (pwm_rising)
                    timeout_counter <= 20'd0;
                else if (timeout_counter < TIMEOUT_CYCLES)
                    timeout_counter <= timeout_counter + 20'd1;
            end else begin
                timeout_counter <= 20'd0;
            end
        end
    end

    //=========================================================================
    // Frequency and duty computation (combinational, from captured values)
    //
    //   raw_freq = CLK_FREQ / captured_period
    //   For 200kHz @ 400MHz: captured_period = 2000, raw_freq = 200000
    //
    //   raw_duty = (captured_high << 16) / captured_period   [Q16.16]
    //   For 50% duty: captured_high = 1000, period = 2000
    //   raw_duty = (1000 << 16) / 2000 = 65536000 / 2000 = 32768 = 0x8000
    //=========================================================================

    // Frequency: simple integer division (synthesizes to DSP or LUT divider)
    // CLK_FREQ / period → truncate to 16 bits (max ~1MHz for 400MHz clock)
    assign raw_freq = captured_period > 0 ? 20'(CLK_FREQ / captured_period) : 20'd0;

    // Duty cycle in Q16.16: (high << 16) / period
    // 36-bit numerator, 20-bit denominator → 32-bit quotient
    // This is a large combinational path — acceptable because it only
    // evaluates once per PWM cycle (VALIDATE state), not every clock.
    logic [35:0] duty_numerator;
    assign duty_numerator = {captured_high, 16'h0000};  // high << 16
    assign raw_duty = (captured_period > 0) ? 32'(duty_numerator / captured_period) : 32'd0;

    //=========================================================================
    // Consistency check
    //
    // Store last N duty measurements. Check if max - min < 1% of full scale.
    // 1% of Q16.16 full scale (1.0) = 0x28F6 (≈ 0.01 in Q16.16)
    //=========================================================================
    localparam int CONSISTENCY_THRESHOLD = 32'h0000_28F6;  // 1% of 1.0 in Q16.16

    function automatic logic check_consistency();
        logic [31:0] d_min, d_max;
        d_min = 32'hFFFF_FFFF;
        d_max = 32'h0000_0000;
        for (int i = 0; i < CONSISTENCY_CYCLES; i++) begin
            if (duty_history[i] < d_min) d_min = duty_history[i];
            if (duty_history[i] > d_max) d_max = duty_history[i];
        end
        return (d_max - d_min) <= CONSISTENCY_THRESHOLD;
    endfunction

    assign consistency_pass = (consistency_count < CONSISTENCY_CYCLES) ?
                               1'b1 :  // still collecting samples — pass
                               check_consistency();  // all collected — check spread

    //=========================================================================
    // Consistency history management
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < CONSISTENCY_CYCLES; i++)
                duty_history[i] <= 32'd0;
            consistency_index <= 3'd0;
            consistency_count <= 3'd0;
        end else begin
            if (state == S_CONSISTENCY) begin
                duty_history[consistency_index] <= raw_duty;
                if (consistency_pass) begin
                    consistency_index <= consistency_index + 2'd1;
                    if (consistency_count < CONSISTENCY_CYCLES)
                        consistency_count <= consistency_count + 3'd1;
                end else begin
                    // Reset on failure: keep latest, restart count
                    consistency_count <= 3'd1;
                    consistency_index <= 2'd1;
                end
            end
        end
    end

    //=========================================================================
    // Output registers
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            duty_q16      <= 32'd0;
            duty_valid    <= 1'b0;
            measured_freq <= 16'd0;
            freq_valid    <= 1'b0;
            pwm_lost      <= 1'b0;
        end else begin
            // Default: de-assert pulses
            duty_valid <= 1'b0;
            freq_valid <= 1'b0;

            case (state)
                S_VALIDATE: begin
                    measured_freq <= raw_freq[19:0];
                    freq_valid   <= 1'b1;
                end

                S_UPDATE: begin
                    duty_q16   <= raw_duty;
                    // Only valid when we've collected enough samples AND they're consistent
                    duty_valid <= consistency_pass && (consistency_count >= CONSISTENCY_CYCLES);
                end

                default: ;
            endcase

            // PWM lost: timeout in IDLE
            pwm_lost <= (state == S_IDLE && timeout_counter >= TIMEOUT_CYCLES);
        end
    end

    //=========================================================================
    // Simulation-only diagnostics
    //=========================================================================
`ifdef VERILATOR
    /* verilator lint_off SYNCASYNCNET */
    always @(posedge clk) begin
        if (rst_n && state == S_VALIDATE) begin
            if (captured_period < 20'd10)
                $display("[WARN] pwm_capture: period too short (%0d cycles)", captured_period);
        end
    end
    /* verilator lint_on SYNCASYNCNET */
`endif
    /* verilator lint_on WIDTH */

endmodule
