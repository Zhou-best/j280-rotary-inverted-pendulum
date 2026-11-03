module encoder_quad_reader #(
    parameter integer CLK_FREQ_HZ   = 50_000_000,
    parameter integer FILTER_CYCLES = 8,
    parameter integer CPR           = 1040,
    parameter integer USE_Z_INDEX   = 0,
    parameter integer REVERSE_DIR   = 0
)(
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   enc_a_raw,
    input  wire                   enc_b_raw,
    input  wire                   enc_z_raw,
    input  wire                   clear_pos,
    input  wire                   calc_en,


    output reg  signed [31:0]     pulse_count,
    output reg  signed [31:0]     alpha_rad_q16,
    output reg  signed [31:0]     dalpha_rad_s_q16,
    output reg  signed [31:0]     speed_pps,
    output reg                    dir_flag
);


    reg [1:0] a_sync_reg;
    reg [1:0] b_sync_reg;
    reg [1:0] z_sync_reg;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_sync_reg <= 2'b00;
            b_sync_reg <= 2'b00;
            z_sync_reg <= 2'b00;
        end else begin
            a_sync_reg <= {a_sync_reg[0], enc_a_raw};
            b_sync_reg <= {b_sync_reg[0], enc_b_raw};
            z_sync_reg <= {z_sync_reg[0], enc_z_raw};
        end
    end


    reg [7:0] a_filter_cnt;
    reg [7:0] b_filter_cnt;
    reg       a_filt;
    reg       b_filt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_filter_cnt <= 8'd0;
            b_filter_cnt <= 8'd0;
            a_filt       <= 1'b0;
            b_filt       <= 1'b0;
        end else begin

            if (a_sync_reg[1] == a_filt) begin
                a_filter_cnt <= 8'd0;
            end else if (a_filter_cnt >= FILTER_CYCLES - 1) begin
                a_filt       <= a_sync_reg[1];
                a_filter_cnt <= 8'd0;
            end else begin
                a_filter_cnt <= a_filter_cnt + 1'b1;
            end


            if (b_sync_reg[1] == b_filt) begin
                b_filter_cnt <= 8'd0;
            end else if (b_filter_cnt >= FILTER_CYCLES - 1) begin
                b_filt       <= b_sync_reg[1];
                b_filter_cnt <= 8'd0;
            end else begin
                b_filter_cnt <= b_filter_cnt + 1'b1;
            end
        end
    end


    reg a_filt_dly, b_filt_dly;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_filt_dly <= 1'b0;
            b_filt_dly <= 1'b0;
        end else begin
            a_filt_dly <= a_filt;
            b_filt_dly <= b_filt;
        end
    end


    wire a_rise = (a_filt == 1'b1) && (a_filt_dly == 1'b0);
    wire a_fall = (a_filt == 1'b0) && (a_filt_dly == 1'b1);
    wire b_rise = (b_filt == 1'b1) && (b_filt_dly == 1'b0);
    wire b_fall = (b_filt == 1'b0) && (b_filt_dly == 1'b1);


    reg signed [1:0] count_step;

    always @(*) begin
        case ({a_filt_dly, b_filt_dly, a_filt, b_filt})

            4'b00_10, 4'b10_11, 4'b11_01, 4'b01_00: count_step =  2'sd1;

            4'b00_01, 4'b01_11, 4'b11_10, 4'b10_00: count_step = -2'sd1;

            default: count_step = 2'sd0;
        endcase
    end


    reg z_sync_dly;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) z_sync_dly <= 1'b0;
        else z_sync_dly <= z_sync_reg[1];
    end
    wire z_rise = (z_sync_reg[1] == 1'b1) && (z_sync_dly == 1'b0);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pulse_count <= 32'sd0;
            dir_flag    <= 1'b0;
        end else if (clear_pos || (USE_Z_INDEX && z_rise)) begin
            pulse_count <= 32'sd0;
        end else begin
            if (REVERSE_DIR == 0) begin
                pulse_count <= pulse_count + count_step;
                if (count_step > 0) dir_flag <= 1'b0;
                else if (count_step < 0) dir_flag <= 1'b1;
            end else begin
                pulse_count <= pulse_count - count_step;
                if (count_step > 0) dir_flag <= 1'b1;
                else if (count_step < 0) dir_flag <= 1'b0;
            end
        end
    end


    localparam integer SHIFT_A = 8;
    localparam integer SHIFT_V = 2;

    localparam signed [17:0] K_ANGLE_18        = (32'sd411775 * (1<<SHIFT_A)) / CPR;
    localparam signed [17:0] K_VEL_18          = (32'sd411775 * 1000) / (CPR * (1<<SHIFT_V));
    localparam signed [17:0] ALPHA_MIN_Q16    = 18'sd1235;
    localparam signed [17:0] ALPHA_SLOPE_Q16  = 18'sd412;
    localparam signed [17:0] ALPHA_MAX_Q16    = 18'sd9830;
    localparam signed [17:0] ALPHA_D_Q16      = 18'sd7314;

    reg signed [31:0] pc_hist [0:1];
    reg signed [31:0] delta_pulse;
    reg signed [31:0] raw_dalpha_q16;
    integer           kk;

    wire signed [31:0] delta_pulse_now = pulse_count - pc_hist[0];

    wire signed [17:0] pulse_cnt_18 = (pulse_count > 32'sd131071) ? 18'sd131071 :
                                      ((pulse_count < -32'sd131072) ? -18'sd131072 : pulse_count[17:0]);


    wire signed [17:0] delta_cnt_18 = (delta_pulse_now > 32'sd4095) ? 18'sd4095 :
                                      ((delta_pulse_now < -32'sd4096) ? -18'sd4096 : delta_pulse_now[17:0]);


    wire signed [35:0] pulse_mult_36    = pulse_cnt_18 * K_ANGLE_18;
    wire signed [35:0] delta_mult_36    = delta_cnt_18 * K_VEL_18;
    wire signed [31:0] alpha_shifted    = pulse_mult_36 >>> SHIFT_A;

    wire signed [35:0] delta_mult_shift = delta_mult_36 <<< SHIFT_V;
    wire signed [31:0] raw_dalpha_calc  = delta_mult_shift[31:0];


    wire signed [31:0] edx_diff         = raw_dalpha_q16 - edx_q16;
    wire signed [17:0] edx_diff_18      = (edx_diff > 32'sd131071) ? 18'sd131071 :
                                          ((edx_diff < -32'sd131072) ? -18'sd131072 : edx_diff[17:0]);
    wire signed [35:0] edx_mult         = edx_diff_18 * ALPHA_D_Q16;
    wire signed [31:0] edx_next         = edx_q16 + (edx_mult >>> 16);

    wire signed [31:0] edx_abs          = (edx_q16 < 0) ? -edx_q16 : edx_q16;
    wire [12:0]       edx_abs_small     = edx_abs[21:8];
    wire signed [35:0] slope_mult       = $signed({5'd0, edx_abs_small}) * ALPHA_SLOPE_Q16;
    wire signed [31:0] alpha_calc       = {{14{ALPHA_MIN_Q16[17]}}, ALPHA_MIN_Q16} + (slope_mult >>> 8);
    wire signed [31:0] alpha_limited    = (alpha_calc > ALPHA_MAX_Q16) ? ALPHA_MAX_Q16 : alpha_calc;


    wire signed [31:0] diff_for_iir     = raw_dalpha_q16 - dalpha_rad_s_q16;
    wire signed [17:0] diff_iir_18      = (diff_for_iir > 32'sd131071) ? 18'sd131071 :
                                          ((diff_for_iir < -32'sd131072) ? -18'sd131072 : diff_for_iir[17:0]);
    wire signed [35:0] iir_diff_mult    = diff_iir_18 * alpha_q16;
    reg  signed [35:0] iir_diff_mult_r;
    wire signed [31:0] iir_diff_shifted = iir_diff_mult_r >>> 16;
    reg  signed [31:0] edx_q16;
    reg  signed [31:0] alpha_q16;
    reg  [2:0]         filter_step;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (kk = 0; kk < 2; kk = kk + 1) pc_hist[kk] <= 32'sd0;
            delta_pulse      <= 32'sd0;
            raw_dalpha_q16   <= 32'sd0;
            alpha_rad_q16    <= 32'sd0;
            dalpha_rad_s_q16 <= 32'sd0;
            speed_pps        <= 32'sd0;
            iir_diff_mult_r  <= 36'sd0;
            edx_q16          <= 32'sd0;
            alpha_q16        <= 32'sd1235;
            filter_step      <= 3'd0;
        end else if (clear_pos || (USE_Z_INDEX && z_rise)) begin
            for (kk = 0; kk < 2; kk = kk + 1) pc_hist[kk] <= 32'sd0;
            delta_pulse      <= 32'sd0;
            alpha_rad_q16    <= 32'sd0;
            raw_dalpha_q16   <= 32'sd0;
            dalpha_rad_s_q16 <= 32'sd0;
            speed_pps        <= 32'sd0;
            edx_q16          <= 32'sd0;
            alpha_q16        <= 32'sd1235;
            filter_step      <= 3'd0;
        end else if (calc_en) begin

            delta_pulse      <= delta_pulse_now;
            pc_hist[0]       <= pulse_count;
            speed_pps        <= delta_pulse_now * 32'sd1000;
            alpha_rad_q16    <= alpha_shifted[31:0];
            raw_dalpha_q16   <= raw_dalpha_calc;
            filter_step      <= 3'd1;
        end else if (filter_step == 3'd1) begin

            edx_q16          <= edx_next;
            alpha_q16        <= alpha_limited;
            filter_step      <= 3'd2;
        end else if (filter_step == 3'd2) begin

            iir_diff_mult_r  <= iir_diff_mult;
            filter_step      <= 3'd3;
        end else if (filter_step == 3'd3) begin

            dalpha_rad_s_q16 <= dalpha_rad_s_q16 + iir_diff_shifted[31:0];
            filter_step      <= 3'd0;
        end
    end

endmodule
