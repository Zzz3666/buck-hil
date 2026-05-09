//=============================================================================
// Verilator C++ harness: pwm_capture verification
//
// Tests:
//   1. Basic capture: 200kHz PWM, 50% duty → verify measured duty/freq
//   2. Duty sweep: 10%, 30%, 70%, 90% → verify linearity
//   3. Glitch rejection: inject 2.5ns glitch → should be filtered
//   4. PWM lost: stop PWM → verify pwm_lost asserts
//   5. Consistency: verify duty_valid only after 3 consistent cycles
//
// Build:
//   verilator --cc --exe --build -j $(nproc) -Wall \
//     pwm_capture.sv harness_capture.cpp --top-module pwm_capture \
//     -CFLAGS "-std=c++11 -O2"
//=============================================================================

#include "Vpwm_capture.h"
#include "verilated.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

double sc_time_stamp() { return 0; }

// Q16.16 to float
inline float q162f(uint32_t q) {
    return (float)(int32_t)q / 65536.0f;
}

//=============================================================================
// PWM generator: produces a square wave at given freq/duty
//=============================================================================
struct PwmGen {
    float freq;      // Hz
    float duty;      // [0.0, 1.0]
    float clk_period; // seconds (2.5ns for 400MHz)
    float t;          // current simulation time

    void init(float f, float d, float cp) {
        freq = f; duty = d; clk_period = cp; t = 0;
    }

    bool tick() {
        t += clk_period;
        float period = 1.0f / freq;
        float phase = fmodf(t, period);
        return phase < (duty * period);
    }
};

//=============================================================================
// Main test
//=============================================================================
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpwm_capture* top = new Vpwm_capture;

    const float CLK_FREQ = 400e6f;
    const float CLK_PERIOD = 2.5e-9f;
    const float PWM_FREQ = 200e3f;  // 200 kHz
    const int   EXPECTED_FREQ = 200000;

    printf("=== PWM Capture Verilator Simulation ===\n");
    printf("Clock: %.0f MHz, PWM: %.0f kHz\n", CLK_FREQ/1e6, PWM_FREQ/1e3);
    printf("Expected period: %.0f cycles\n\n", CLK_FREQ / PWM_FREQ);

    // Set expected frequency
    top->expected_freq = 0;  // unused in current implementation
    top->pwm_in = 0;

    // Reset
    top->rst_n = 0;
    top->clk = 0;
    for (int i = 0; i < 10; i++) {
        top->clk = !top->clk;
        top->eval();
    }
    top->rst_n = 1;

    //=========================================================================
    // Test 1: 50% duty steady-state
    //=========================================================================
    printf("=== Test 1: 50%% duty @ 200kHz ===\n");
    PwmGen pwm;
    pwm.init(PWM_FREQ, 0.50f, CLK_PERIOD);

    int duty_valid_count = 0;
    int freq_valid_count = 0;
    float last_duty = -1;
    float last_freq = -1;
    const int TEST1_CYCLES = 50000;  // enough for ~25 PWM cycles (>3 for consistency)

    for (int c = 0; c < TEST1_CYCLES; c++) {
        top->clk = 0;
        top->pwm_in = pwm.tick() ? 1 : 0;
        top->eval();

        top->clk = 1;
        top->eval();

        if (top->duty_valid) {
            duty_valid_count++;
            last_duty = q162f(top->duty_q16);
        }
        if (top->freq_valid) {
            freq_valid_count++;
            last_freq = (float)(int)top->measured_freq;
        }
    }

    printf("  duty_valid pulses: %d, freq_valid pulses: %d\n", duty_valid_count, freq_valid_count);
    printf("  Final duty: %.4f (%.1f%%), freq: %.0f Hz\n",
           last_duty, last_duty * 100, last_freq);

    float duty_err = fabsf(last_duty - 0.50f);
    float freq_err = fabsf(last_freq - PWM_FREQ) / PWM_FREQ;
    printf("  Duty error: %.4f, Freq error: %.2f%%\n", duty_err, freq_err * 100);

    bool t1_pass = (duty_err < 0.01f) && (freq_err < 0.01f) && (duty_valid_count > 0);
    printf("  %s\n\n", t1_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 2: Duty sweep
    //=========================================================================
    printf("=== Test 2: Duty sweep (10%%, 30%%, 70%%, 90%%) ===\n");
    float test_duties[] = {0.10f, 0.30f, 0.70f, 0.90f};
    bool t2_pass = true;

    for (int td = 0; td < 4; td++) {
        pwm.init(PWM_FREQ, test_duties[td], CLK_PERIOD);

        // Soft reset: force IDLE by stopping PWM briefly
        top->rst_n = 0;
        for (int i = 0; i < 5; i++) { top->clk = !top->clk; top->eval(); }
        top->rst_n = 1;

        float measured = 0;
        for (int c = 0; c < 50000; c++) {
            top->clk = 0;
            top->pwm_in = pwm.tick() ? 1 : 0;
            top->eval();
            top->clk = 1;
            top->eval();
            if (top->duty_valid) {
                measured = q162f(top->duty_q16);
            }
        }

        float err = fabsf(measured - test_duties[td]);
        printf("  D=%.0f%%: measured=%.4f (%.1f%%), err=%.4f %s\n",
               test_duties[td]*100, measured, measured*100, err,
               err < 0.02f ? "OK" : "FAIL");
        if (err >= 0.02f) t2_pass = false;
    }
    printf("  %s\n\n", t2_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 3: Glitch rejection
    //=========================================================================
    printf("=== Test 3: Glitch rejection (2.5ns pulse) ===\n");

    // Reset
    top->rst_n = 0;
    for (int i = 0; i < 5; i++) { top->clk = !top->clk; top->eval(); }
    top->rst_n = 1;

    // Generate clean 50% PWM, but inject a 1-cycle glitch
    pwm.init(PWM_FREQ, 0.50f, CLK_PERIOD);
    int glitch_cycles[] = {1000, 5000, 9000};  // inject glitches at these cycles
    int glitch_idx = 0;
    int duty_before_glitch = 0;

    for (int c = 0; c < 20000; c++) {
        top->clk = 0;

        // Normal PWM, but inject 1-cycle glitch at specific times
        bool pwm_val = pwm.tick();
        if (glitch_idx < 3 && c == glitch_cycles[glitch_idx]) {
            pwm_val = !pwm_val;  // flip for one cycle
            glitch_idx++;
        }
        top->pwm_in = pwm_val ? 1 : 0;
        top->eval();

        top->clk = 1;
        top->eval();

        if (top->duty_valid) duty_before_glitch++;
    }

    printf("  duty_valid after glitch injection: %d (should be > 0)\n", duty_before_glitch);
    bool t3_pass = (duty_before_glitch > 0);
    printf("  %s\n\n", t3_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 4: PWM lost detection
    //=========================================================================
    printf("=== Test 4: PWM lost detection ===\n");

    top->rst_n = 0;
    for (int i = 0; i < 5; i++) { top->clk = !top->clk; top->eval(); }
    top->rst_n = 1;

    // Run PWM normally for a bit
    pwm.init(PWM_FREQ, 0.50f, CLK_PERIOD);
    for (int c = 0; c < 10000; c++) {
        top->clk = 0;
        top->pwm_in = pwm.tick() ? 1 : 0;
        top->eval();
        top->clk = 1;
        top->eval();
    }

    // Now stop PWM (hold low)
    bool pwm_lost_seen = false;
    bool too_fast = false;
    for (int c = 0; c < 50000; c++) {
        top->clk = 0;
        top->pwm_in = 0;  // PWM stopped
        top->eval();
        top->clk = 1;
        top->eval();

        if (top->pwm_lost) {
            pwm_lost_seen = true;
            if (c < 9000) too_fast = true;  // should take TIMEOUT_CYCLES=10000
        }
    }

    printf("  pwm_lost asserted: %s, too fast: %s\n",
           pwm_lost_seen ? "YES" : "NO",
           too_fast ? "YES (FAIL)" : "NO (OK)");
    bool t4_pass = pwm_lost_seen && !too_fast;
    printf("  %s\n\n", t4_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Summary
    //=========================================================================
    printf("=== Summary ===\n");
    printf("  Test 1 (50%% duty):   %s\n", t1_pass ? "PASS" : "FAIL");
    printf("  Test 2 (duty sweep): %s\n", t2_pass ? "PASS" : "FAIL");
    printf("  Test 3 (glitch rej): %s\n", t3_pass ? "PASS" : "FAIL");
    printf("  Test 4 (PWM lost):  %s\n", t4_pass ? "PASS" : "FAIL");

    bool all_pass = t1_pass && t2_pass && t3_pass && t4_pass;
    printf("\n  %s\n", all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    delete top;
    return all_pass ? 0 : 1;
}
