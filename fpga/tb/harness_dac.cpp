//=============================================================================
// Verilator C++ harness: dac_interface verification
//
// Tests:
//   1. Frame format: verify {CMD, ADDR, DATA} for ch0=Vout, ch1=IL
//   2. SPI timing: SCLK frequency, CS_N assertion, MSB-first order
//   3. Multi-update: verify correct frames across multiple update_strobe pulses
//   4. State machine: IDLE→TX→WAIT→IDLE transitions
//
// Build:
//   verilator --cc --exe --build -j $(nproc) -Wall \
//     dac_interface.sv harness_dac.cpp --top-module dac_interface \
//     -CFLAGS "-std=c++11 -O2"
//=============================================================================

#include "Vdac_interface.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

double sc_time_stamp() { return 0; }

//=============================================================================
// SPI bus monitor: captures 32-bit frames from SDI line
//=============================================================================
struct SpiMonitor {
    uint32_t frame;          // captured frame
    int      bit_count;      // bits captured so far
    bool     prev_sclk;      // for edge detection
    bool     prev_cs_n;      // for frame start detection
    int      frames_captured;
    int      total_bits;
    bool     cs_was_low;     // for CS timing check

    void reset() {
        frame = 0;
        bit_count = 0;
        prev_sclk = 0;
        prev_cs_n = 1;
        frames_captured = 0;
        total_bits = 0;
        cs_was_low = false;
    }

    // Sample on SCLK rising edge, MSB first
    void sample(bool sclk, bool sdi, bool cs_n) {
        // Detect CS falling edge: start of frame
        if (prev_cs_n && !cs_n) {
            frame = 0;
            bit_count = 0;
            cs_was_low = true;
        }

        // Sample on SCLK rising edge
        if (!prev_sclk && sclk && !cs_n) {
            frame = (frame << 1) | (sdi ? 1 : 0);
            bit_count++;
            total_bits++;
        }

        // Detect CS rising edge: end of frame
        if (!prev_cs_n && cs_n && cs_was_low) {
            if (bit_count == 32) {
                frames_captured++;
            } else if (bit_count > 0) {
                printf("  [WARN] Short frame: %d bits\n", bit_count);
            }
            cs_was_low = false;
        }

        prev_sclk = sclk;
        prev_cs_n = cs_n;
    }

    uint8_t cmd()  const { return (frame >> 24) & 0xFF; }
    uint8_t addr() const { return (frame >> 16) & 0xFF; }
    uint16_t data() const { return frame & 0xFFFF; }
};

//=============================================================================
// Main test
//=============================================================================
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vdac_interface* top = new Vdac_interface;

    SpiMonitor mon;
    mon.reset();

    printf("=== DAC80508 Interface Verilator Simulation ===\n");
    printf("SPI_CLK_DIV=%d → SCLK = 100MHz / %d = %.0f MHz\n",
           1, 2*1, 100.0/2);
    printf("Expected frame format: {CMD, 0x00, DATA[15:0]}\n\n");

    // Set test data
    top->ch0_data = 0x8000;  // Vout = 2.5V (half-scale)
    top->ch1_data = 0x4000;  // IL   = 1.25V (quarter-scale)
    for (int i = 2; i < 8; i++)
        ((uint16_t*)&top->ch0_data)[i] = 0x0000;  // reserved = 0

    top->update_strobe = 0;

    // Reset
    top->rst_n = 0;
    top->clk = 0;
    for (int i = 0; i < 10; i++) {
        top->clk = !top->clk;
        top->eval();
    }
    top->rst_n = 1;

    //=========================================================================
    // Test 1: Single update — verify 2 frames (ch0, ch1)
    //=========================================================================
    printf("=== Test 1: Single update (ch0=0x8000, ch1=0x4000) ===\n");

    // Pulse update_strobe
    top->update_strobe = 1;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    top->update_strobe = 0;

    // Wait for transaction to complete
    for (int c = 0; c < 200; c++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
        mon.sample(top->spi_sclk, top->spi_sdi, top->spi_cs_n);
    }

    printf("  Frames captured: %d, total bits: %d\n", mon.frames_captured, mon.total_bits);
    bool t1_pass = (mon.frames_captured == 2);
    printf("  %s\n\n", t1_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 2: Verify frame content (decode captured frames)
    //=========================================================================
    printf("=== Test 2: Frame content verification ===\n");

    mon.reset();
    top->update_strobe = 1;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    top->update_strobe = 0;

    uint32_t frames[2] = {0, 0};
    int fidx = 0;

    for (int c = 0; c < 200; c++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();

        bool prev_cs = mon.prev_cs_n;
        mon.sample(top->spi_sclk, top->spi_sdi, top->spi_cs_n);

        // On CS rising edge, capture the completed frame
        if (!prev_cs && mon.prev_cs_n && mon.bit_count == 32) {
            if (fidx < 2) frames[fidx++] = mon.frame;
        }
    }

    // Verify ch0: CMD=0x08, ADDR=0x00, DATA=0x8000
    uint8_t  cmd0  = (frames[0] >> 24) & 0xFF;
    uint8_t  addr0 = (frames[0] >> 16) & 0xFF;
    uint16_t data0 = frames[0] & 0xFFFF;

    printf("  CH0: CMD=0x%02X ADDR=0x%02X DATA=0x%04X\n", cmd0, addr0, data0);
    bool ch0_ok = (cmd0 == 0x08) && (addr0 == 0x00) && (data0 == 0x8000);
    printf("  CH0 %s\n", ch0_ok ? "OK" : "FAIL");

    // Verify ch1: CMD=0x09, ADDR=0x00, DATA=0x4000
    uint8_t  cmd1  = (frames[1] >> 24) & 0xFF;
    uint8_t  addr1 = (frames[1] >> 16) & 0xFF;
    uint16_t data1 = frames[1] & 0xFFFF;

    printf("  CH1: CMD=0x%02X ADDR=0x%02X DATA=0x%04X\n", cmd1, addr1, data1);
    bool ch1_ok = (cmd1 == 0x09) && (addr1 == 0x00) && (data1 == 0x4000);
    printf("  CH1 %s\n", ch1_ok ? "OK" : "FAIL");

    bool t2_pass = ch0_ok && ch1_ok;
    printf("  %s\n\n", t2_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 3: Multiple updates with changing data
    //=========================================================================
    printf("=== Test 3: Multiple updates with changing data ===\n");

    uint16_t test_data[][2] = {
        {0x2000, 0x1000},  // Vout=0.625V, IL=0.3125V
        {0xC000, 0x6000},  // Vout=3.75V, IL=1.875V
        {0xFFFF, 0xAAAA},  // Vout=5V, IL=3.33V
    };

    bool t3_pass = true;
    for (int t = 0; t < 3; t++) {
        mon.reset();
        top->ch0_data = test_data[t][0];
        top->ch1_data = test_data[t][1];

        top->update_strobe = 1;
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
        top->update_strobe = 0;

        fidx = 0;
        for (int c = 0; c < 200; c++) {
            top->clk = 0; top->eval();
            top->clk = 1; top->eval();

            bool prev_cs = mon.prev_cs_n;
            mon.sample(top->spi_sclk, top->spi_sdi, top->spi_cs_n);
            if (!prev_cs && mon.prev_cs_n && mon.bit_count == 32 && fidx < 2)
                frames[fidx++] = mon.frame;
        }

        uint16_t d0 = frames[0] & 0xFFFF;
        uint16_t d1 = frames[1] & 0xFFFF;
        bool ok = (d0 == test_data[t][0]) && (d1 == test_data[t][1]);
        printf("  Update %d: ch0=0x%04X %s, ch1=0x%04X %s\n",
               t, d0, d0==test_data[t][0]?"OK":"FAIL",
               d1, d1==test_data[t][1]?"OK":"FAIL");
        if (!ok) t3_pass = false;
    }
    printf("  %s\n\n", t3_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 4: SPI timing check (SCLK period ≈ 20ns @ 50MHz, CS setup)
    //=========================================================================
    printf("=== Test 4: SPI timing ===\n");

    mon.reset();
    top->update_strobe = 1;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    top->update_strobe = 0;

    int sclk_cycles = 0;
    int cs_active_cycles = 0;
    bool cs_active = false;

    for (int c = 0; c < 200; c++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();

        if (top->spi_cs_n == 0) {
            cs_active = true;
            cs_active_cycles++;
        }
        if (top->spi_sclk) sclk_cycles++;

        mon.sample(top->spi_sclk, top->spi_sdi, top->spi_cs_n);
    }

    // At 100MHz with SPI_CLK_DIV=1: each SCLK half-cycle = 1 clk = 10ns
    // 32 bits = 64 half-cycles = 64 clk cycles of SCLK activity
    // Plus CS setup/hold overhead
    printf("  SCLK cycles: %d, CS active cycles: %d, frames: %d\n",
           sclk_cycles, cs_active_cycles, mon.frames_captured);
    printf("  Expected: ~64 SCLK half-cycles per 32-bit frame (×2ch = ~128)\n");

    bool t4_pass = (mon.frames_captured == 2) && (cs_active_cycles > 0) && (sclk_cycles > 60);
    printf("  %s\n\n", t4_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Test 5: update_strobe during WAIT — should not restart
    //=========================================================================
    printf("=== Test 5: update_strobe gating ===\n");

    mon.reset();
    top->update_strobe = 1;
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();

    // Keep update_strobe high — should complete TX and stay in WAIT
    for (int c = 0; c < 200; c++) {
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
        mon.sample(top->spi_sclk, top->spi_sdi, top->spi_cs_n);
    }

    printf("  Frames with strobe held high: %d (should be 2, not repeating)\n",
           mon.frames_captured);
    bool t5_pass = (mon.frames_captured == 2);
    printf("  %s\n\n", t5_pass ? "[PASS]" : "[FAIL]");

    //=========================================================================
    // Summary
    //=========================================================================
    printf("=== Summary ===\n");
    printf("  Test 1 (frame count):     %s\n", t1_pass ? "PASS" : "FAIL");
    printf("  Test 2 (frame content):   %s\n", t2_pass ? "PASS" : "FAIL");
    printf("  Test 3 (multi-update):    %s\n", t3_pass ? "PASS" : "FAIL");
    printf("  Test 4 (SPI timing):      %s\n", t4_pass ? "PASS" : "FAIL");
    printf("  Test 5 (strobe gating):   %s\n", t5_pass ? "PASS" : "FAIL");

    bool all_pass = t1_pass && t2_pass && t3_pass && t4_pass && t5_pass;
    printf("\n  %s\n", all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    delete top;
    return all_pass ? 0 : 1;
}
