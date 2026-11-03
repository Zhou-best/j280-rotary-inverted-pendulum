module j280_cmd_rx #(
    parameter integer CLK_FREQ = 50_000_000,
    parameter integer BAUD     = 115200,
    parameter integer ALIVE_MS = 2000,
    parameter integer ACK_MS   = 100,
    parameter integer UNIT_MS  = 100,
    parameter integer TOL_MS   = 40
)(
    input  wire       clk,
    input  wire       rst_n,
    input  wire       uart_rx,

    output reg  [2:0] mode_sel,
    output reg        mode_valid,
    output reg        stop_pulse,
    output reg  [7:0] last_cmd,
    output reg  [7:0] last_arg,
    output reg        link_alive,
    output reg        cmd_ack
);

    localparam integer CLKS_PER_MS  = CLK_FREQ / 1000;
    localparam integer ALIVE_CYCLES = CLKS_PER_MS * ALIVE_MS;
    localparam integer ACK_UNIT     = CLKS_PER_MS * ACK_MS;


    localparam integer W0_LO = UNIT_MS * 1 - TOL_MS;
    localparam integer W0_HI = UNIT_MS * 1 + TOL_MS;
    localparam integer W1_LO = UNIT_MS * 2 - TOL_MS;
    localparam integer W1_HI = UNIT_MS * 2 + TOL_MS;
    localparam integer W2_LO = UNIT_MS * 3 - TOL_MS;
    localparam integer W2_HI = UNIT_MS * 3 + TOL_MS;
    localparam integer W3_LO = UNIT_MS * 4 - TOL_MS;
    localparam integer W3_HI = UNIT_MS * 4 + TOL_MS;
    localparam integer W4_LO = UNIT_MS * 5 - TOL_MS;
    localparam integer W4_HI = UNIT_MS * 5 + TOL_MS;
    localparam integer W5_LO = UNIT_MS * 6 - TOL_MS;
    localparam integer W5_HI = UNIT_MS * 6 + TOL_MS;


    reg in_s1, in_s2, in_s3;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in_s1 <= 1'b1;
            in_s2 <= 1'b1;
            in_s3 <= 1'b1;
        end else begin
            in_s1 <= uart_rx;
            in_s2 <= in_s1;
            in_s3 <= in_s2;
        end
    end

    wire rise_edge = in_s2 & ~in_s3;
    wire fall_edge = ~in_s2 & in_s3;


    reg [15:0] ms_div;
    reg        ms_tick;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ms_div  <= 16'd0;
            ms_tick <= 1'b0;
        end else if (ms_div >= CLKS_PER_MS - 1) begin
            ms_div  <= 16'd0;
            ms_tick <= 1'b1;
        end else begin
            ms_div  <= ms_div + 16'd1;
            ms_tick <= 1'b0;
        end
    end


    reg [15:0] width_ms;
    reg [15:0] width_lat;
    reg        width_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            width_ms    <= 16'd0;
            width_lat   <= 16'd0;
            width_valid <= 1'b0;
        end else begin
            width_valid <= 1'b0;

            if (rise_edge) begin
                width_ms <= 16'd0;
            end else if (in_s2 && ms_tick) begin
                if (width_ms < 16'd1023)
                    width_ms <= width_ms + 16'd1;
            end

            if (fall_edge) begin
                width_lat   <= width_ms;
                width_valid <= 1'b1;
            end
        end
    end


    reg [2:0] dec_mode;
    reg       dec_ok;

    always @(*) begin
        dec_ok   = 1'b1;
        dec_mode = 3'd0;

        if      (width_lat >= W0_LO && width_lat <= W0_HI) dec_mode = 3'd0;
        else if (width_lat >= W1_LO && width_lat <= W1_HI) dec_mode = 3'd1;
        else if (width_lat >= W2_LO && width_lat <= W2_HI) dec_mode = 3'd2;
        else if (width_lat >= W3_LO && width_lat <= W3_HI) dec_mode = 3'd3;
        else if (width_lat >= W4_LO && width_lat <= W4_HI) dec_mode = 3'd4;
        else if (width_lat >= W5_LO && width_lat <= W5_HI) dec_mode = 3'd5;
        else begin
            dec_ok   = 1'b0;
            dec_mode = 3'd0;
        end
    end


    reg ack_load;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mode_sel   <= 3'd0;
            mode_valid <= 1'b0;
            stop_pulse <= 1'b0;
            last_cmd   <= 8'd0;
            last_arg   <= 8'd0;
            ack_load   <= 1'b0;
        end else begin
            mode_valid <= 1'b0;
            stop_pulse <= 1'b0;
            ack_load   <= 1'b0;

            if (width_valid && dec_ok) begin
                mode_sel   <= dec_mode;
                mode_valid <= 1'b1;
                last_cmd   <= 8'h10;
                last_arg   <= {5'd0, dec_mode};
                ack_load   <= 1'b1;
                if (dec_mode == 3'd0)
                    stop_pulse <= 1'b1;
            end
        end
    end


    reg [25:0] ack_cnt;
    reg [25:0] ack_width;

    always @(*) begin
        case (mode_sel)
            3'd0:    ack_width = ACK_UNIT * 1;
            3'd1:    ack_width = ACK_UNIT * 2;
            3'd2:    ack_width = ACK_UNIT * 3;
            3'd3:    ack_width = ACK_UNIT * 4;
            3'd4:    ack_width = ACK_UNIT * 5;
            default: ack_width = ACK_UNIT * 6;
        endcase
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ack_cnt <= 26'd0;
            cmd_ack <= 1'b0;
        end else if (ack_load) begin
            ack_cnt <= ack_width;
            cmd_ack <= 1'b1;
        end else if (ack_cnt != 26'd0) begin
            ack_cnt <= ack_cnt - 26'd1;
            cmd_ack <= 1'b1;
        end else begin
            cmd_ack <= 1'b0;
        end
    end


    reg [31:0] alive_cnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            alive_cnt  <= 32'd0;
            link_alive <= 1'b0;
        end else if (width_valid && dec_ok) begin
            alive_cnt  <= 32'd0;
            link_alive <= 1'b1;
        end else if (alive_cnt < ALIVE_CYCLES) begin
            alive_cnt  <= alive_cnt + 32'd1;
            link_alive <= 1'b1;
        end else begin
            link_alive <= 1'b0;
        end
    end

endmodule
