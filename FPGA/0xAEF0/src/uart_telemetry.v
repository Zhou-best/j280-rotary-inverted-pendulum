module uart_byte_tx #(
    parameter integer BAUD_CNT_MAX = 434
)(
    input  wire       clk,
    input  wire       rst_n,
    input  wire [7:0] data_in,
    input  wire       send_req,
    output reg        tx_out,
    output reg        send_done,
    output wire       tx_ready
);
    localparam ST_IDLE = 2'd0, ST_START = 2'd1, ST_DATA = 2'd2, ST_STOP = 2'd3;

    reg [1:0]  st;
    reg [15:0] baud_cnt;
    reg [2:0]  bit_idx;
    reg [7:0]  data_r;

    assign tx_ready = (st == ST_IDLE);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st        <= ST_IDLE;
            baud_cnt  <= 16'd0;
            bit_idx   <= 3'd0;
            data_r    <= 8'd0;
            tx_out    <= 1'b1;
            send_done <= 1'b0;
        end else begin
            send_done <= 1'b0;
            case (st)
                ST_IDLE: begin
                    tx_out <= 1'b1;
                    if (send_req) begin
                        data_r   <= data_in;
                        baud_cnt <= 16'd0;
                        st       <= ST_START;
                    end
                end
                ST_START: begin
                    tx_out <= 1'b0;
                    if (baud_cnt == BAUD_CNT_MAX - 1) begin
                        baud_cnt <= 16'd0;
                        bit_idx  <= 3'd0;
                        st       <= ST_DATA;
                    end else begin
                        baud_cnt <= baud_cnt + 16'd1;
                    end
                end
                ST_DATA: begin
                    tx_out <= data_r[bit_idx];
                    if (baud_cnt == BAUD_CNT_MAX - 1) begin
                        baud_cnt <= 16'd0;
                        if (bit_idx == 3'd7) st <= ST_STOP;
                        else                 bit_idx <= bit_idx + 3'd1;
                    end else begin
                        baud_cnt <= baud_cnt + 16'd1;
                    end
                end
                ST_STOP: begin
                    tx_out <= 1'b1;
                    if (baud_cnt == BAUD_CNT_MAX - 1) begin
                        baud_cnt  <= 16'd0;
                        send_done <= 1'b1;
                        st        <= ST_IDLE;
                    end else begin
                        baud_cnt <= baud_cnt + 16'd1;
                    end
                end
                default: st <= ST_IDLE;
            endcase
        end
    end
endmodule


module uart_telemetry #(
    parameter integer CLK_FREQ_HZ = 50_000_000,
    parameter integer BAUD        = 115_200,
    parameter integer PERIOD_MS   = 10
)(
    input  wire               clk,
    input  wire               rst_n,


    input  wire signed [31:0] theta_err_q16,
    input  wire signed [31:0] dtheta_q16,
    input  wire signed [31:0] alpha_q16,
    input  wire signed [31:0] dalpha_q16,
    input  wire signed [15:0] duty,
    input  wire [1:0]         fsm_state,
    input  wire               lqr_en,
    input  wire               swing_en,
    input  wire               motor_active,
    input  wire               energy_deficit,
    input  wire               calib_done,
    input  wire               traj_moving,

    output wire               uart_tx
);
    localparam integer BAUD_CNT_MAX = CLK_FREQ_HZ / BAUD;
    localparam integer TICK_MAX     = (CLK_FREQ_HZ / 1000) * PERIOD_MS;
    localparam integer FRAME_LEN    = 22;


    reg [31:0] tick_cnt;
    reg        tick;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tick_cnt <= 32'd0;
            tick     <= 1'b0;
        end else begin
            tick <= 1'b0;
            if (tick_cnt >= TICK_MAX - 1) begin
                tick_cnt <= 32'd0;
                tick     <= 1'b1;
            end else begin
                tick_cnt <= tick_cnt + 32'd1;
            end
        end
    end


    reg signed [31:0] lat_theta, lat_dth, lat_alpha, lat_dalpha;
    reg signed [15:0] lat_duty;
    reg [7:0]         lat_status, lat_ck;


    wire [7:0] status_now = {traj_moving, calib_done, energy_deficit, motor_active,
                             swing_en, lqr_en, fsm_state};


    function [7:0] frame_xor(input signed [31:0] a, input signed [31:0] b,
                             input signed [31:0] c, input signed [31:0] d,
                             input signed [15:0] e, input [7:0] s);
        frame_xor = a[7:0]^a[15:8]^a[23:16]^a[31:24] ^
                    b[7:0]^b[15:8]^b[23:16]^b[31:24] ^
                    c[7:0]^c[15:8]^c[23:16]^c[31:24] ^
                    d[7:0]^d[15:8]^d[23:16]^d[31:24] ^
                    e[7:0]^e[15:8] ^ s;
    endfunction

    wire [7:0] ck_now = frame_xor(theta_err_q16, dtheta_q16, alpha_q16,
                                  dalpha_q16, duty, status_now);


    function [7:0] frame_byte(input [4:0] i);
        case (i)
            5'd0:  frame_byte = 8'hAA;
            5'd1:  frame_byte = 8'h55;
            5'd2:  frame_byte = lat_theta[7:0];
            5'd3:  frame_byte = lat_theta[15:8];
            5'd4:  frame_byte = lat_theta[23:16];
            5'd5:  frame_byte = lat_theta[31:24];
            5'd6:  frame_byte = lat_dth[7:0];
            5'd7:  frame_byte = lat_dth[15:8];
            5'd8:  frame_byte = lat_dth[23:16];
            5'd9:  frame_byte = lat_dth[31:24];
            5'd10: frame_byte = lat_alpha[7:0];
            5'd11: frame_byte = lat_alpha[15:8];
            5'd12: frame_byte = lat_alpha[23:16];
            5'd13: frame_byte = lat_alpha[31:24];
            5'd14: frame_byte = lat_dalpha[7:0];
            5'd15: frame_byte = lat_dalpha[15:8];
            5'd16: frame_byte = lat_dalpha[23:16];
            5'd17: frame_byte = lat_dalpha[31:24];
            5'd18: frame_byte = lat_duty[7:0];
            5'd19: frame_byte = lat_duty[15:8];
            5'd20: frame_byte = lat_status;
            5'd21: frame_byte = lat_ck;
            default: frame_byte = 8'h00;
        endcase
    endfunction


    reg        sending;
    reg [4:0]  idx;
    reg [7:0]  tx_data;
    reg        tx_req;
    wire       tx_ready;
    wire       tx_done;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sending  <= 1'b0;
            idx      <= 5'd0;
            tx_data  <= 8'd0;
            tx_req   <= 1'b0;
            lat_theta <= 32'sd0; lat_dth <= 32'sd0;
            lat_alpha <= 32'sd0; lat_dalpha <= 32'sd0;
            lat_duty  <= 16'sd0; lat_status <= 8'd0; lat_ck <= 8'd0;
        end else begin
            tx_req <= 1'b0;
            if (!sending) begin
                if (tick) begin
                    lat_theta  <= theta_err_q16;
                    lat_dth    <= dtheta_q16;
                    lat_alpha  <= alpha_q16;
                    lat_dalpha <= dalpha_q16;
                    lat_duty   <= duty;
                    lat_status <= status_now;
                    lat_ck     <= ck_now;
                    sending <= 1'b1;
                    idx     <= 5'd0;
                end
            end else begin
                if (tx_done) begin
                    if (idx == FRAME_LEN - 1) sending <= 1'b0;
                    else                      idx     <= idx + 5'd1;
                end else if (tx_ready && !tx_req) begin
                    tx_req  <= 1'b1;
                    tx_data <= frame_byte(idx);
                end
            end
        end
    end

    uart_byte_tx #(
        .BAUD_CNT_MAX (BAUD_CNT_MAX)
    ) u_byte_tx (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (tx_data),
        .send_req  (tx_req),
        .tx_out    (uart_tx),
        .send_done (tx_done),
        .tx_ready  (tx_ready)
    );

endmodule
