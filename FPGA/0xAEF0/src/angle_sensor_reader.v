module angle_sensor_reader #(
    parameter integer CLK_FREQ_HZ   = 50_000_000,
    parameter integer SPI_SCLK_HZ   = 2_500_000,
    parameter integer ADC_RESOLUTION= 12,
    parameter integer INVERT_DIR    = 0,


    parameter integer USE_SPI       = 1
)(
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   calc_en,


    output reg                    spi_cs_n,
    output reg                    spi_sclk,
    input  wire                   spi_miso,


    input  wire [11:0]            zero_offset_raw,


    input  wire                   ext_raw_valid,
    input  wire [11:0]            ext_raw_data,


    output reg  [11:0]            raw_adc_data,
    output reg  signed [31:0]     theta_err_q16,
    output reg  signed [31:0]     dtheta_q16,
    output reg                    sample_done
);


    localparam integer SCLK_DIV = CLK_FREQ_HZ / (SPI_SCLK_HZ * 2);
    reg [7:0] sclk_cnt;
    reg       sclk_pulse;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sclk_cnt   <= 8'd0;
            sclk_pulse <= 1'b0;
        end else begin
            if (sclk_cnt >= SCLK_DIV - 1) begin
                sclk_cnt   <= 8'd0;
                sclk_pulse <= 1'b1;
            end else begin
                sclk_cnt   <= sclk_cnt + 1'b1;
                sclk_pulse <= 1'b0;
            end
        end
    end


    localparam STATE_IDLE    = 3'd0;
    localparam STATE_START   = 3'd1;
    localparam STATE_XFER    = 3'd2;
    localparam STATE_FINISH  = 3'd3;

    reg [2:0]  spi_state;
    reg [4:0]  bit_index;
    reg [15:0] shift_reg;
    reg [11:0] adc_latch;
    reg        adc_valid_internal;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spi_state          <= STATE_IDLE;
            spi_cs_n           <= 1'b1;
            spi_sclk           <= 1'b0;
            bit_index          <= 5'd0;
            shift_reg          <= 16'd0;
            adc_latch          <= 12'd0;
            adc_valid_internal <= 1'b0;
        end else begin
            adc_valid_internal <= 1'b0;

            case (spi_state)
                STATE_IDLE: begin
                    spi_cs_n <= 1'b1;
                    spi_sclk <= 1'b0;


                    if (calc_en && (USE_SPI != 0)) begin
                        spi_cs_n  <= 1'b0;
                        bit_index <= 5'd0;
                        shift_reg <= 16'd0;
                        spi_state <= STATE_START;
                    end
                end

                STATE_START: begin
                    if (sclk_pulse) begin
                        spi_sclk  <= 1'b0;
                        spi_state <= STATE_XFER;
                    end
                end

                STATE_XFER: begin
                    if (sclk_pulse) begin
                        spi_sclk <= ~spi_sclk;

                        if (~spi_sclk == 1'b1) begin
                            shift_reg <= {shift_reg[14:0], spi_miso};
                            bit_index <= bit_index + 1'b1;
                            if (bit_index >= 5'd15) begin
                                spi_state <= STATE_FINISH;
                            end
                        end
                    end
                end

                STATE_FINISH: begin
                    if (sclk_pulse) begin
                        spi_cs_n           <= 1'b1;
                        spi_sclk           <= 1'b0;

                        adc_latch          <= shift_reg[11:0];
                        adc_valid_internal <= (USE_SPI != 0);
                        spi_state          <= STATE_IDLE;
                    end
                end

                default: spi_state <= STATE_IDLE;
            endcase
        end
    end


    wire        process_trigger = adc_valid_internal | ext_raw_valid;
    wire [11:0] active_raw_adc  = ext_raw_valid ? ext_raw_data : adc_latch;


    reg signed [12:0] diff_raw;
    reg signed [12:0] diff_unwrapped;

    always @(*) begin
        diff_raw = $signed({1'b0, active_raw_adc}) - $signed({1'b0, zero_offset_raw});
        if (diff_raw > 13'sd2047) begin
            diff_unwrapped = diff_raw - 13'sd4096;
        end else if (diff_raw < -13'sd2048) begin
            diff_unwrapped = diff_raw + 13'sd4096;
        end else begin
            diff_unwrapped = diff_raw;
        end
    end


    localparam signed [31:0] SCALE_NUM_Q16    = 32'sd411775;


    localparam signed [31:0] ALPHA_MIN_Q16   = 32'sd1235;
    localparam signed [31:0] ALPHA_SLOPE_Q16 = 32'sd824;
    localparam signed [31:0] ALPHA_MAX_Q16   = 32'sd5243;
    localparam signed [31:0] ALPHA_D_Q16     = 32'sd7314;


    wire signed [63:0] diff_scaled_64     = $signed({{51{diff_unwrapped[12]}}, diff_unwrapped}) * $signed(SCALE_NUM_Q16);
    wire signed [63:0] diff_rad_shifted   = diff_scaled_64 >>> 12;
    wire signed [31:0] diff_rad_q16       = diff_rad_shifted[31:0];

    reg signed [31:0] theta_prev_q16;
    reg signed [31:0] diff_raw_r;
    reg signed [31:0] diff_step_r;


    reg signed [31:0] raw_dtheta_q16;
    reg signed [31:0] edx_q16;
    reg signed [31:0] alpha_q16;
    reg        [2:0]  filter_step;


    wire signed [31:0] current_theta_q16  = (INVERT_DIR == 0) ? diff_rad_q16 : (-diff_rad_q16);
    wire signed [31:0] diff_theta_raw     = current_theta_q16 - theta_prev_q16;


    wire signed [31:0] diff_theta_step    = (diff_raw_r >  32'sd205887) ? (diff_raw_r - 32'sd411775) :
                                           ((diff_raw_r < -32'sd205888) ? (diff_raw_r + 32'sd411775) :
                                                                          diff_raw_r);

    wire signed [47:0] raw_dtheta_mult    = $signed(diff_step_r) * 32'sd1000;
    wire signed [31:0] raw_dtheta_calc    = raw_dtheta_mult[31:0];


    localparam signed [31:0] DTHETA_PHYS_LIMIT_Q16 = 32'sd2293760;
    wire signed [31:0] raw_dtheta_clamped = (raw_dtheta_calc > DTHETA_PHYS_LIMIT_Q16) ? DTHETA_PHYS_LIMIT_Q16 :
                                           ((raw_dtheta_calc < -DTHETA_PHYS_LIMIT_Q16) ? -DTHETA_PHYS_LIMIT_Q16 :
                                            raw_dtheta_calc);


    wire signed [31:0] edx_diff       = raw_dtheta_q16 - edx_q16;
    wire signed [47:0] edx_mult       = edx_diff * ALPHA_D_Q16;
    wire signed [31:0] edx_next       = edx_q16 + (edx_mult >>> 16);


    wire signed [31:0] edx_abs        = (edx_q16 < 0) ? -edx_q16 : edx_q16;
    wire [12:0]       edx_abs_small   = edx_abs[21:8];
    wire signed [47:0] slope_mult     = $signed({2'b0, edx_abs_small}) * ALPHA_SLOPE_Q16;
    wire signed [31:0] alpha_calc     = ALPHA_MIN_Q16 + (slope_mult >>> 8);
    wire signed [31:0] alpha_limited  = (alpha_calc > ALPHA_MAX_Q16) ? ALPHA_MAX_Q16 : alpha_calc;


    wire signed [31:0] iir_diff       = raw_dtheta_q16 - dtheta_q16;
    wire signed [47:0] iir_mult       = iir_diff * alpha_q16;
    wire signed [31:0] iir_step_q16   = iir_mult >>> 16;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            raw_adc_data     <= 12'd0;
            theta_err_q16    <= 32'sd0;
            theta_prev_q16   <= 32'sd0;
            diff_raw_r       <= 32'sd0;
            diff_step_r      <= 32'sd0;
            raw_dtheta_q16   <= 32'sd0;
            dtheta_q16       <= 32'sd0;
            edx_q16          <= 32'sd0;
            alpha_q16        <= ALPHA_MIN_Q16;
            sample_done      <= 1'b0;
            filter_step      <= 3'd0;
        end else if (filter_step == 3'd1) begin

            diff_step_r  <= diff_theta_step;
            filter_step  <= 3'd2;
            sample_done  <= 1'b0;
        end else if (filter_step == 3'd2) begin

            raw_dtheta_q16 <= raw_dtheta_clamped;
            filter_step    <= 3'd3;
            sample_done    <= 1'b0;
        end else if (filter_step == 3'd3) begin

            edx_q16         <= edx_next;
            alpha_q16       <= alpha_limited;
            filter_step     <= 3'd4;
            sample_done     <= 1'b0;
        end else if (filter_step == 3'd4) begin

            dtheta_q16     <= dtheta_q16 + iir_step_q16;
            filter_step    <= 3'd0;
            sample_done    <= 1'b1;
        end else if (process_trigger) begin

            raw_adc_data   <= active_raw_adc;
            theta_err_q16  <= current_theta_q16;
            theta_prev_q16 <= current_theta_q16;
            diff_raw_r     <= diff_theta_raw;
            filter_step    <= 3'd1;
            sample_done    <= 1'b0;
        end else begin
            sample_done    <= 1'b0;
        end
    end

endmodule
