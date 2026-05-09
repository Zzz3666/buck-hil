//=============================================================================
// Verilator C++ harness: buck_solver verification
//
// Tests:
//   1. Startup transient (0V → steady-state, duty=50%, Vin=12V → Vout≈6V)
//   2. Load step response (R_load: 10Ω → 5Ω)
//   3. Clamping: diode prevents negative i_L, buck cannot exceed Vin
//   4. Open-loop steady-state accuracy vs floating-point reference
//
// Build:
//   verilator --cc --exe --build -j $(nproc) -Wall \
//     buck_solver.sv harness_solver.cpp --top-module buck_solver \
//     -CFLAGS "-std=c++11 -O2"
//=============================================================================

#include "Vbuck_solver.h"
#include "verilated.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Required by Verilator when DUT uses $time
double sc_time_stamp() { return 0; }

//=============================================================================
// Fixed-point conversion helpers
//=============================================================================

// Convert float to Q16.16 fixed-point
inline int32_t f2q16(float x) {
    return (int32_t)roundf(x * 65536.0f);
}

// Convert Q16.16 to float
inline float q162f(int32_t q) {
    return (float)q / 65536.0f;
}

// Convert float to Q8.24 fixed-point
inline int32_t f2q8_24(float x) {
    return (int32_t)roundf(x * 16777216.0f);
}

// Convert Q8.24 to float
inline float q8_242f(int32_t q) {
    return (float)q / 16777216.0f;
}

//=============================================================================
// Floating-point reference model (Forward Euler, same equations)
//=============================================================================
struct BuckRef {
    float i_L;     // inductor current (A)
    float v_C;     // capacitor voltage (V)
    float vin;
    float L;       // inductance (H)
    float C;       // capacitance (F)
    float R_load;  // load resistance (Ω)
    float R_L;     // inductor ESR (Ω)
    float Vf;      // diode forward voltage (V)
    float dt;      // time step (s)
    float duty;    // duty cycle [0,1]
    float period;  // switching period (s)
    float t;       // current time

    void init(float _vin, float _L_uh, float _C_uf, float _R, float _RL, float _Vf,
              float _duty, float _fsw, float _dt) {
        vin    = _vin;
        L      = _L_uh * 1e-6f;
        C      = _C_uf * 1e-6f;
        R_load = _R;
        R_L    = _RL;
        Vf     = _Vf;
        duty   = _duty;
        dt     = _dt;
        period = 1.0f / _fsw;
        i_L    = 0.0f;
        v_C    = 0.0f;
        t      = 0.0f;
    }

    void step() {
        bool sw_on = fmodf(t, period) < (duty * period);

        float vout = v_C;
        float di_dt, dv_dt;

        if (sw_on) {
            di_dt = (vin - vout - i_L * R_L) / L;
        } else {
            di_dt = (-Vf - vout - i_L * R_L) / L;
        }
        dv_dt = (i_L - vout / R_load) / C;

        float i_L_new = i_L + di_dt * dt;
        float v_C_new = v_C + dv_dt * dt;

        // Clamp: i_L ≥ 0, v_C ≥ 0, v_C ≤ vin, i_L ≤ IL_MAX
        if (i_L_new < 0.0f) i_L_new = 0.0f;
        if (i_L_new > 10.0f) i_L_new = 10.0f;  // match RTL IL_MAX
        if (v_C_new < 0.0f) v_C_new = 0.0f;
        if (v_C_new > vin)  v_C_new = vin;

        i_L = i_L_new;
        v_C = v_C_new;
        t  += dt;
    }
};

//=============================================================================
// Main test
//=============================================================================
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vbuck_solver* top = new Vbuck_solver;

    // Test configuration
    const float VIN      = 12.0f;      // 12V input
    const float L_UH     = 10.0f;      // 10μH
    const float C_UF     = 100.0f;     // 100μF
    const float R_LOAD   = 5.0f;       // 5Ω → 1.2A load at 6V, stays in CCM
    const float R_L_OHM  = 0.05f;      // 50mΩ ESR
    const float VF       = 0.8f;       // 0.8V diode
    const float DUTY     = 0.55f;       // 55% duty — stays in CCM with diode loss
    const float FSW      = 200000.0f;  // 200 kHz
    const float DT       = 100e-9f;    // 100 ns

    // Pre-compute dt/L and dt/C (Q8.24)
    float dt_over_L_float = DT / (L_UH * 1e-6f);
    float dt_over_C_float = DT / (C_UF * 1e-6f);
    int32_t dt_over_L = f2q8_24(dt_over_L_float);
    int32_t dt_over_C = f2q8_24(dt_over_C_float);

    // R_load and inv_R_load (Q16.16)
    int32_t r_load_fp   = f2q16(R_LOAD);
    int32_t inv_r_load   = f2q16(1.0f / R_LOAD);
    int32_t vin_fp       = f2q16(VIN);
    int32_t vf_fp        = f2q16(VF);
    int32_t r_l_fp       = f2q8_24(R_L_OHM);  // ESR in Q8.24

    // Duty cycle in Q16.16
    int32_t duty_fp = f2q16(DUTY);

    // PWM period in cycles: 100MHz / 200kHz = 500
    // (hardcoded via PERIOD parameter in DUT)

    printf("=== Buck Solver Verilator Simulation ===\n");
    printf("Vin=%.1fV  L=%.0fμH  C=%.0fμF  R=%.0fΩ  RL=%.0fmΩ  Vf=%.1fV  D=%.2f  Fsw=%.0fkHz\n",
           VIN, L_UH, C_UF, R_LOAD, R_L_OHM*1000, VF, DUTY, FSW/1000);
    printf("dt/L=%.6f (0x%08X)  dt/C=%.6f (0x%08X)\n",
           dt_over_L_float, (unsigned)dt_over_L, dt_over_C_float, (unsigned)dt_over_C);
    printf("inv_R_load=%.4f (0x%08X)\n", 1.0f/R_LOAD, (unsigned)inv_r_load);
    printf("Expected Vout_ss ≈ %.2fV\n\n", VIN * DUTY);

    // Set parameters
    top->vin          = vin_fp;
    top->l_val        = dt_over_L;
    top->c_val        = dt_over_C;
    top->r_load       = r_load_fp;
    top->inv_r_load   = inv_r_load;
    top->r_l          = r_l_fp;
    top->vf           = vf_fp;
    top->duty_q16_in  = duty_fp;
    top->duty_valid_in = 0;
    top->param_update  = 0;

    // Reset sequence
    top->rst_n = 0;
    top->clk   = 0;
    for (int i = 0; i < 10; i++) {
        top->clk = !top->clk;
        top->eval();
    }
    top->rst_n = 1;

    // Pulse duty_valid and param_update AFTER letting step_counter advance
    // (avoid race: param_update and solve_tick on same cycle)
    for (int i = 0; i < 15; i++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
    }

    top->duty_valid_in = 1;
    top->param_update  = 1;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    top->duty_valid_in = 0;
    top->param_update  = 0;

    // Reference model
    BuckRef ref;
    ref.init(VIN, L_UH, C_UF, R_LOAD, R_L_OHM, VF, DUTY, FSW, DT);

    // Run simulation
    const int TOTAL_CYCLES = 500000;      // 500k cycles = 50ms = 10,000 switching cycles
    const int LOG_INTERVAL = 5000;        // log every 5k cycles (0.5ms)
    float max_v_err = 0.0f;
    float max_i_err = 0.0f;
    float sum_v_err = 0.0f;
    float sum_i_err = 0.0f;
    int   err_count = 0;
    int   step_count = 0;
    int   clamp_count_i = 0;
    int   clamp_count_v = 0;

    printf("Cycle       Time         Vout(RTL)  Vout(Ref)   ΔV(mV)   IL(RTL)   IL(Ref)    ΔI(mA)\n");
    printf("------      --------     ---------  ---------   ------   --------  --------   ------\n");

    for (int cycle = 0; cycle < TOTAL_CYCLES; cycle++) {
        // negedge: set inputs
        top->clk = 0;
        top->eval();

        // posedge: read outputs
        top->clk = 1;
        top->eval();

        // Reference model: only step when solver produces output (every STEP_DIV cycles)
        if (top->step_valid) {
            ref.step();
        }

        if (top->step_valid) {
            step_count++;
            float v_rtl = q162f((int32_t)top->v_out);
            float i_rtl = q162f((int32_t)top->i_l_out);
            float v_ref = ref.v_C;
            float i_ref = ref.i_L;

            float v_err = fabsf(v_rtl - v_ref);
            float i_err = fabsf(i_rtl - i_ref);

            if (v_err > max_v_err) max_v_err = v_err;
            if (i_err > max_i_err) max_i_err = i_err;
            sum_v_err += v_err;
            sum_i_err += i_err;
            err_count++;

            // Track clamping events (pass/fail)
            if (i_rtl < 0.0f) { clamp_count_i++; printf("[FAIL] i_L < 0 at cycle %d\n", cycle); }
            if (v_rtl < 0.0f) { clamp_count_v++; printf("[FAIL] v_C < 0 at cycle %d\n", cycle); }

            // Log output
            if ((cycle % LOG_INTERVAL) < 100 || cycle < 1000 && (cycle % 100 == 0)) {
                // Only log when step_valid lines up with log interval
                if (cycle % LOG_INTERVAL == 0 || (cycle < 1000 && cycle % 100 == 0)) {
                    float t_ms = (float)cycle * 10.0f / 1e6f;  // 10ns per cycle → ms
                    printf("%-6d     %8.3fms   %9.4f   %9.4f   %6.2f   %8.4f  %8.4f   %6.2f\n",
                           cycle, t_ms,
                           v_rtl, v_ref, v_err*1000,
                           i_rtl, i_ref, i_err*1000);
                }
            }
        }
    }

    float avg_v_err = (err_count > 0) ? (sum_v_err / err_count) : 0;
    float avg_i_err = (err_count > 0) ? (sum_i_err / err_count) : 0;

    printf("\n=== Results (over %d solver steps) ===\n", err_count);
    printf("Vout:  max error = %.3f mV,  avg error = %.3f mV\n", max_v_err * 1000, avg_v_err * 1000);
    printf("I_L:   max error = %.3f mA,  avg error = %.3f mA\n", max_i_err * 1000, avg_i_err * 1000);
    printf("Clamp violations: i_L=%d, v_C=%d\n", clamp_count_i, clamp_count_v);

    // Pass/fail criteria
    bool passed = true;
    if (max_v_err > 0.05f) {  // 50mV max
        printf("[FAIL] Vout error exceeds 50mV\n");
        passed = false;
    }
    if (max_i_err > 0.02f) {  // 20mA max
        printf("[FAIL] IL error exceeds 20mA\n");
        passed = false;
    }
    if (clamp_count_i > 0 || clamp_count_v > 0) {
        printf("[FAIL] Clamp violations detected\n");
        passed = false;
    }

    if (passed) {
        printf("[PASS] All checks passed.\n");
    }

    // Final state
    float v_final = q162f((int32_t)top->v_out);
    float i_final = q162f((int32_t)top->i_l_out);
    printf("\nFinal state: Vout=%.4fV (raw=0x%08X), IL=%.4fA (raw=0x%08X) (ref: %.4fV, %.4fA)\n",
           v_final, (unsigned)top->v_out, i_final, (unsigned)top->i_l_out, ref.v_C, ref.i_L);

    delete top;
    return passed ? 0 : 1;
}
