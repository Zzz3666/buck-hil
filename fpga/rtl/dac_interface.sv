//=============================================================================
// Module: dac_interface
// Purpose: SPI master for DAC80508 — simple counter-based 32-bit SPI.
//          SCLK = clk / (2 * SPI_CLK_DIV), CS_N low during transfer.
//
// Clock:     100 MHz → SCLK = 50 MHz (SPI_CLK_DIV=1)
// Frame:     32-bit MSB first: {CMD[7:0], ADDR[7:0], DATA[15:0]}
// Channels:  ch0=Vout (CMD=0x08), ch1=IL (CMD=0x09)
//
// State machine: IDLE → TX_CH0 → TX_CH1 → WAIT → IDLE
//=============================================================================

module dac_interface #(
    parameter int SPI_CLK_DIV  = 1,         // SCLK = clk / (2*SPI_CLK_DIV)
    parameter int NUM_CHANNELS = 2          // active channels
) (
    input  logic        clk,           // 100 MHz
    input  logic        rst_n,

    input  logic [15:0] ch0_data,      // Vout DAC code
    input  logic [15:0] ch1_data,      // IL   DAC code
    /* verilator lint_off UNUSED */
    input  logic [15:0] ch2_data,      // reserved
    input  logic [15:0] ch3_data,
    input  logic [15:0] ch4_data,
    input  logic [15:0] ch5_data,
    input  logic [15:0] ch6_data,
    input  logic [15:0] ch7_data,
    /* verilator lint_on UNUSED */
    input  logic        update_strobe,

    output logic        spi_sclk,
    output logic        spi_sdi,
    output logic        spi_cs_n,
    output logic        spi_ldac_n     // tied low = auto-update
);

    // DAC80508 command bytes: DAC0_DATA=0x08 ... DAC7_DATA=0x0F
    localparam logic [7:0] DAC_CMD [0:7] = '{
        8'h08, 8'h09, 8'h0A, 8'h0B, 8'h0C, 8'h0D, 8'h0E, 8'h0F
    };

    //=========================================================================
    // State machine
    //=========================================================================
    typedef enum logic [2:0] {S_IDLE, S_TX_CH0, S_GAP0, S_GAP0a, S_TX_CH1, S_GAP1, S_GAP1a, S_WAIT} state_t;
    state_t state;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state <= S_IDLE;
        else begin
            case (state)
                S_IDLE:   if (update_strobe) state <= S_TX_CH0;
                S_TX_CH0: if (bit_cnt_done)  state <= S_GAP0;
                S_GAP0:                      state <= S_GAP0a;
                S_GAP0a:                     state <= S_TX_CH1;
                S_TX_CH1: if (bit_cnt_done)  state <= S_GAP1;
                S_GAP1:                      state <= S_GAP1a;
                S_GAP1a:                     state <= S_WAIT;
                S_WAIT:   if (!update_strobe) state <= S_IDLE;
                default:  state <= S_IDLE;
            endcase
        end
    end

    //=========================================================================
    // SPI clock divider and bit counter
    // 32 bits per frame, each bit = 2 * SPI_CLK_DIV clk cycles
    //=========================================================================
    localparam int BITS_PER_FRAME = 32;
    localparam int CYCLES_PER_BIT = 2 * SPI_CLK_DIV;
    localparam int TOTAL_CYCLES   = BITS_PER_FRAME * CYCLES_PER_BIT;

    logic [$clog2(TOTAL_CYCLES)-1:0] cycle_counter;  // counts 0..TOTAL_CYCLES-1
    logic                             bit_cnt_done;   // last cycle of current frame

    /* verilator lint_off WIDTH */  // small counters vs int params
    assign bit_cnt_done = (cycle_counter == TOTAL_CYCLES - 1);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cycle_counter <= '0;
        end else begin
            if (state == S_TX_CH0 || state == S_TX_CH1) begin
                if (bit_cnt_done)
                    cycle_counter <= '0;
                else
                    cycle_counter <= cycle_counter + 1;
            end else begin
                cycle_counter <= '0;
            end
        end
    end

    //=========================================================================
    // SCLK generation: toggles every half-bit period
    // With SPI_CLK_DIV=1: toggles every cycle → 50 MHz square wave during TX
    //=========================================================================
    logic half_bit_tick;
    assign half_bit_tick = (cycle_counter % (CYCLES_PER_BIT/2) == 0);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spi_sclk <= 1'b0;
        end else begin
            if (state == S_TX_CH0 || state == S_TX_CH1) begin
                if (half_bit_tick)
                    spi_sclk <= ~spi_sclk;
            end else begin
                spi_sclk <= 1'b0;
            end
        end
    end

    //=========================================================================
    // SPI data
    logic [31:0] shift_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shift_reg <= 32'd0;
        end else begin
            if (state == S_IDLE && update_strobe) begin
                shift_reg <= {DAC_CMD[0], 8'h00, ch0_data};
            end else if (state == S_GAP0a) begin
                shift_reg <= {DAC_CMD[1], 8'h00, ch1_data};  // load ch1 during gap
            end else if ((state == S_TX_CH0 || state == S_TX_CH1) &&
                         (cycle_counter % CYCLES_PER_BIT == (CYCLES_PER_BIT/2))) begin
                // Shift on SCLK falling edge (mid-point of bit period)
                shift_reg <= {shift_reg[30:0], 1'b0};
            end
        end
    end

    assign spi_sdi = shift_reg[31];

    //=========================================================================
    // CS_N: low during TX, high otherwise
    //=========================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spi_cs_n <= 1'b1;
        end else begin
            spi_cs_n <= (state != S_TX_CH0 && state != S_TX_CH1 &&
                         !(state == S_IDLE && update_strobe));
        end
    end

    //=========================================================================
    // LDAC_N: tied low for auto-update on CS_N rising
    //=========================================================================
    assign spi_ldac_n = 1'b0;

    /* verilator lint_on WIDTH */

endmodule
