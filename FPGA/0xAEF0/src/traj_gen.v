module traj_gen #(
    parameter integer CLK_FREQ_HZ      = 50_000_000,
    parameter integer UPDATE_HZ        = 1000,
    parameter integer TIMER_1MS_LIMIT  = 50_000_000 / 1000
)(
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   calc_en,
    input  wire                   sync_phase,
    input  wire [1:0]             mode_sel,

    output reg  signed [31:0]     alpha_ref_q16,
    output reg  signed [31:0]     dalpha_ref_q16
);


    localparam signed [31:0] POS_45DEG_Q16  = 32'sd51472;
    localparam signed [31:0] NEG_45DEG_Q16  = -32'sd51472;
    localparam signed [31:0] TRAJ_AMP_Q16   = 32'sd28594;
    localparam signed [31:0] TRAJ_VMAX_Q16  = 32'sd35933;


    localparam signed [31:0] RAMP_STEP_Q16  = 32'sd57;
    localparam signed [31:0] RAMP_VEL_Q16   = 32'sd57000;


    reg [12:0] time_cnt_5s;
    reg [6:0]  lut_idx_r;
    reg [1:0]  mode_sel_r;
    reg        stage1_valid;
    reg        sine_phase_active;
    reg signed [31:0] ramp_pos_q16;


    wire [31:0] idx_calc = (time_cnt_5s * 32'd26844) >> 20;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            time_cnt_5s       <= 13'd0;
            lut_idx_r         <= 7'd0;
            mode_sel_r        <= 2'd0;
            stage1_valid      <= 1'b0;
            sine_phase_active <= 1'b0;
        end else if (sync_phase) begin

            time_cnt_5s       <= 13'd0;
            lut_idx_r         <= 7'd0;
            sine_phase_active <= 1'b0;
        end else if (calc_en) begin
            if (mode_sel != 2'b11) begin
                time_cnt_5s       <= 13'd0;
                lut_idx_r         <= 7'd0;
                sine_phase_active <= 1'b0;
            end else if (!sine_phase_active) begin

                time_cnt_5s <= 13'd0;
                lut_idx_r   <= 7'd0;
                if (ramp_pos_q16 == 32'sd0) begin
                    sine_phase_active <= 1'b1;
                end
            end else begin
                if (time_cnt_5s >= 13'd4999) begin
                    time_cnt_5s <= 13'd0;
                    lut_idx_r   <= idx_calc[6:0];
                end else begin
                    time_cnt_5s <= time_cnt_5s + 13'd1;
                    lut_idx_r   <= idx_calc[6:0];
                end
            end

            mode_sel_r   <= mode_sel;
            stage1_valid <= 1'b1;
        end else begin
            stage1_valid <= 1'b0;
        end
    end


    reg signed [15:0] sin_q14;
    reg signed [15:0] cos_q14;

    always @(*) begin
        case (lut_idx_r)
            7'd0:   begin sin_q14 = 16'sd0;     cos_q14 = 16'sd16384; end
            7'd1:   begin sin_q14 = 16'sd804;   cos_q14 = 16'sd16364; end
            7'd2:   begin sin_q14 = 16'sd1606;  cos_q14 = 16'sd16305; end
            7'd3:   begin sin_q14 = 16'sd2404;  cos_q14 = 16'sd16207; end
            7'd4:   begin sin_q14 = 16'sd3196;  cos_q14 = 16'sd16069; end
            7'd5:   begin sin_q14 = 16'sd3981;  cos_q14 = 16'sd15893; end
            7'd6:   begin sin_q14 = 16'sd4756;  cos_q14 = 16'sd15679; end
            7'd7:   begin sin_q14 = 16'sd5520;  cos_q14 = 16'sd15426; end
            7'd8:   begin sin_q14 = 16'sd6270;  cos_q14 = 16'sd15137; end
            7'd9:   begin sin_q14 = 16'sd7005;  cos_q14 = 16'sd14811; end
            7'd10:  begin sin_q14 = 16'sd7723;  cos_q14 = 16'sd14449; end
            7'd11:  begin sin_q14 = 16'sd8423;  cos_q14 = 16'sd14053; end
            7'd12:  begin sin_q14 = 16'sd9102;  cos_q14 = 16'sd13623; end
            7'd13:  begin sin_q14 = 16'sd9760;  cos_q14 = 16'sd13160; end
            7'd14:  begin sin_q14 = 16'sd10394; cos_q14 = 16'sd12665; end
            7'd15:  begin sin_q14 = 16'sd11003; cos_q14 = 16'sd12140; end
            7'd16:  begin sin_q14 = 16'sd11585; cos_q14 = 16'sd11585; end
            7'd17:  begin sin_q14 = 16'sd12140; cos_q14 = 16'sd11003; end
            7'd18:  begin sin_q14 = 16'sd12665; cos_q14 = 16'sd10394; end
            7'd19:  begin sin_q14 = 16'sd13160; cos_q14 = 16'sd9760;  end
            7'd20:  begin sin_q14 = 16'sd13623; cos_q14 = 16'sd9102;  end
            7'd21:  begin sin_q14 = 16'sd14053; cos_q14 = 16'sd8423;  end
            7'd22:  begin sin_q14 = 16'sd14449; cos_q14 = 16'sd7723;  end
            7'd23:  begin sin_q14 = 16'sd14811; cos_q14 = 16'sd7005;  end
            7'd24:  begin sin_q14 = 16'sd15137; cos_q14 = 16'sd6270;  end
            7'd25:  begin sin_q14 = 16'sd15426; cos_q14 = 16'sd5520;  end
            7'd26:  begin sin_q14 = 16'sd15679; cos_q14 = 16'sd4756;  end
            7'd27:  begin sin_q14 = 16'sd15893; cos_q14 = 16'sd3981;  end
            7'd28:  begin sin_q14 = 16'sd16069; cos_q14 = 16'sd3196;  end
            7'd29:  begin sin_q14 = 16'sd16207; cos_q14 = 16'sd2404;  end
            7'd30:  begin sin_q14 = 16'sd16305; cos_q14 = 16'sd1606;  end
            7'd31:  begin sin_q14 = 16'sd16364; cos_q14 = 16'sd804;   end
            7'd32:  begin sin_q14 = 16'sd16384; cos_q14 = 16'sd0;     end
            7'd33:  begin sin_q14 = 16'sd16364; cos_q14 = -16'sd804;  end
            7'd34:  begin sin_q14 = 16'sd16305; cos_q14 = -16'sd1606; end
            7'd35:  begin sin_q14 = 16'sd16207; cos_q14 = -16'sd2404; end
            7'd36:  begin sin_q14 = 16'sd16069; cos_q14 = -16'sd3196; end
            7'd37:  begin sin_q14 = 16'sd15893; cos_q14 = -16'sd3981; end
            7'd38:  begin sin_q14 = 16'sd15679; cos_q14 = -16'sd4756; end
            7'd39:  begin sin_q14 = 16'sd15426; cos_q14 = -16'sd5520; end
            7'd40:  begin sin_q14 = 16'sd15137; cos_q14 = -16'sd6270; end
            7'd41:  begin sin_q14 = 16'sd14811; cos_q14 = -16'sd7005; end
            7'd42:  begin sin_q14 = 16'sd14449; cos_q14 = -16'sd7723; end
            7'd43:  begin sin_q14 = 16'sd14053; cos_q14 = -16'sd8423; end
            7'd44:  begin sin_q14 = 16'sd13623; cos_q14 = -16'sd9102; end
            7'd45:  begin sin_q14 = 16'sd13160; cos_q14 = -16'sd9760; end
            7'd46:  begin sin_q14 = 16'sd12665; cos_q14 = -16'sd10394;end
            7'd47:  begin sin_q14 = 16'sd12140; cos_q14 = -16'sd11003;end
            7'd48:  begin sin_q14 = 16'sd11585; cos_q14 = -16'sd11585;end
            7'd49:  begin sin_q14 = 16'sd11003; cos_q14 = -16'sd12140;end
            7'd50:  begin sin_q14 = 16'sd10394; cos_q14 = -16'sd12665;end
            7'd51:  begin sin_q14 = 16'sd9760;  cos_q14 = -16'sd13160;end
            7'd52:  begin sin_q14 = 16'sd9102;  cos_q14 = -16'sd13623;end
            7'd53:  begin sin_q14 = 16'sd8423;  cos_q14 = -16'sd14053;end
            7'd54:  begin sin_q14 = 16'sd7723;  cos_q14 = -16'sd14449;end
            7'd55:  begin sin_q14 = 16'sd7005;  cos_q14 = -16'sd14811;end
            7'd56:  begin sin_q14 = 16'sd6270;  cos_q14 = -16'sd15137;end
            7'd57:  begin sin_q14 = 16'sd5520;  cos_q14 = -16'sd15426;end
            7'd58:  begin sin_q14 = 16'sd4756;  cos_q14 = -16'sd15679;end
            7'd59:  begin sin_q14 = 16'sd3981;  cos_q14 = -16'sd15893;end
            7'd60:  begin sin_q14 = 16'sd3196;  cos_q14 = -16'sd16069;end
            7'd61:  begin sin_q14 = 16'sd2404;  cos_q14 = -16'sd16207;end
            7'd62:  begin sin_q14 = 16'sd1606;  cos_q14 = -16'sd16305;end
            7'd63:  begin sin_q14 = 16'sd804;   cos_q14 = -16'sd16364;end
            7'd64:  begin sin_q14 = 16'sd0;     cos_q14 = -16'sd16384;end
            7'd65:  begin sin_q14 = -16'sd804;  cos_q14 = -16'sd16364;end
            7'd66:  begin sin_q14 = -16'sd1606; cos_q14 = -16'sd16305;end
            7'd67:  begin sin_q14 = -16'sd2404; cos_q14 = -16'sd16207;end
            7'd68:  begin sin_q14 = -16'sd3196; cos_q14 = -16'sd16069;end
            7'd69:  begin sin_q14 = -16'sd3981; cos_q14 = -16'sd15893;end
            7'd70:  begin sin_q14 = -16'sd4756; cos_q14 = -16'sd15679;end
            7'd71:  begin sin_q14 = -16'sd5520; cos_q14 = -16'sd15426;end
            7'd72:  begin sin_q14 = -16'sd6270; cos_q14 = -16'sd15137;end
            7'd73:  begin sin_q14 = -16'sd7005; cos_q14 = -16'sd14811;end
            7'd74:  begin sin_q14 = -16'sd7723; cos_q14 = -16'sd14449;end
            7'd75:  begin sin_q14 = -16'sd8423; cos_q14 = -16'sd14053;end
            7'd76:  begin sin_q14 = -16'sd9102; cos_q14 = -16'sd13623;end
            7'd77:  begin sin_q14 = -16'sd9760; cos_q14 = -16'sd13160;end
            7'd78:  begin sin_q14 = -16'sd10394;cos_q14 = -16'sd12665;end
            7'd79:  begin sin_q14 = -16'sd11003;cos_q14 = -16'sd12140;end
            7'd80:  begin sin_q14 = -16'sd11585;cos_q14 = -16'sd11585;end
            7'd81:  begin sin_q14 = -16'sd12140;cos_q14 = -16'sd11003;end
            7'd82:  begin sin_q14 = -16'sd12665;cos_q14 = -16'sd10394;end
            7'd83:  begin sin_q14 = -16'sd13160;cos_q14 = -16'sd9760; end
            7'd84:  begin sin_q14 = -16'sd13623;cos_q14 = -16'sd9102; end
            7'd85:  begin sin_q14 = -16'sd14053;cos_q14 = -16'sd8423; end
            7'd86:  begin sin_q14 = -16'sd14449;cos_q14 = -16'sd7723; end
            7'd87:  begin sin_q14 = -16'sd14811;cos_q14 = -16'sd7005; end
            7'd88:  begin sin_q14 = -16'sd15137;cos_q14 = -16'sd6270; end
            7'd89:  begin sin_q14 = -16'sd15426;cos_q14 = -16'sd5520; end
            7'd90:  begin sin_q14 = -16'sd15679;cos_q14 = -16'sd4756; end
            7'd91:  begin sin_q14 = -16'sd15893;cos_q14 = -16'sd3981; end
            7'd92:  begin sin_q14 = -16'sd16069;cos_q14 = -16'sd3196; end
            7'd93:  begin sin_q14 = -16'sd16207;cos_q14 = -16'sd2404; end
            7'd94:  begin sin_q14 = -16'sd16305;cos_q14 = -16'sd1606; end
            7'd95:  begin sin_q14 = -16'sd16364;cos_q14 = -16'sd804;  end
            7'd96:  begin sin_q14 = -16'sd16384;cos_q14 = 16'sd0;     end
            7'd97:  begin sin_q14 = -16'sd16364;cos_q14 = 16'sd804;   end
            7'd98:  begin sin_q14 = -16'sd16305;cos_q14 = 16'sd1606;  end
            7'd99:  begin sin_q14 = -16'sd16207;cos_q14 = 16'sd2404;  end
            7'd100: begin sin_q14 = -16'sd16069;cos_q14 = 16'sd3196;  end
            7'd101: begin sin_q14 = -16'sd15893;cos_q14 = 16'sd3981;  end
            7'd102: begin sin_q14 = -16'sd15679;cos_q14 = 16'sd4756;  end
            7'd103: begin sin_q14 = -16'sd15426;cos_q14 = 16'sd5520;  end
            7'd104: begin sin_q14 = -16'sd15137;cos_q14 = 16'sd6270;  end
            7'd105: begin sin_q14 = -16'sd14811;cos_q14 = 16'sd7005;  end
            7'd106: begin sin_q14 = -16'sd14449;cos_q14 = 16'sd7723;  end
            7'd107: begin sin_q14 = -16'sd14053;cos_q14 = 16'sd8423;  end
            7'd108: begin sin_q14 = -16'sd13623;cos_q14 = 16'sd9102;  end
            7'd109: begin sin_q14 = -16'sd13160;cos_q14 = 16'sd9760;  end
            7'd110: begin sin_q14 = -16'sd12665;cos_q14 = 16'sd10394; end
            7'd111: begin sin_q14 = -16'sd12140;cos_q14 = 16'sd11003; end
            7'd112: begin sin_q14 = -16'sd11585;cos_q14 = 16'sd11585; end
            7'd113: begin sin_q14 = -16'sd11003;cos_q14 = 16'sd12140; end
            7'd114: begin sin_q14 = -16'sd10394;cos_q14 = 16'sd12665; end
            7'd115: begin sin_q14 = -16'sd9760; cos_q14 = 16'sd13160; end
            7'd116: begin sin_q14 = -16'sd9102; cos_q14 = 16'sd13623; end
            7'd117: begin sin_q14 = -16'sd8423; cos_q14 = 16'sd14053; end
            7'd118: begin sin_q14 = -16'sd7723; cos_q14 = 16'sd14449; end
            7'd119: begin sin_q14 = -16'sd7005; cos_q14 = 16'sd14811; end
            7'd120: begin sin_q14 = -16'sd6270; cos_q14 = 16'sd15137; end
            7'd121: begin sin_q14 = -16'sd5520; cos_q14 = 16'sd15426; end
            7'd122: begin sin_q14 = -16'sd4756; cos_q14 = 16'sd15679; end
            7'd123: begin sin_q14 = -16'sd3981; cos_q14 = 16'sd15893; end
            7'd124: begin sin_q14 = -16'sd3196; cos_q14 = 16'sd16069; end
            7'd125: begin sin_q14 = -16'sd2404; cos_q14 = 16'sd16207; end
            7'd126: begin sin_q14 = -16'sd1606; cos_q14 = 16'sd16305; end
            7'd127: begin sin_q14 = -16'sd804;  cos_q14 = 16'sd16364; end
            default:begin sin_q14 = 16'sd0;     cos_q14 = 16'sd16384; end
        endcase
    end


    wire signed [63:0] sine_pos_mult    = $signed(TRAJ_AMP_Q16)  * $signed(sin_q14);
    wire signed [63:0] sine_vel_mult    = $signed(TRAJ_VMAX_Q16) * $signed(cos_q14);
    wire signed [63:0] sine_pos_shifted = sine_pos_mult >>> 14;
    wire signed [63:0] sine_vel_shifted = sine_vel_mult >>> 14;

    wire signed [31:0] sine_pos_q16     = sine_pos_shifted[31:0];
    wire signed [31:0] sine_vel_q16     = sine_vel_shifted[31:0];


    reg signed [31:0] target_setpoint;
    always @(*) begin
        case (mode_sel_r)
            2'b00: target_setpoint = 32'sd0;
            2'b01: target_setpoint = POS_45DEG_Q16;
            2'b10: target_setpoint = NEG_45DEG_Q16;
            default: target_setpoint = 32'sd0;
        endcase
    end

    reg signed [31:0] ramp_vel_q16;
    reg [1:0]         vel_decay_cnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ramp_pos_q16   <= 32'sd0;
            ramp_vel_q16   <= 32'sd0;
            vel_decay_cnt  <= 2'd0;
            alpha_ref_q16  <= 32'sd0;
            dalpha_ref_q16 <= 32'sd0;
        end else if (stage1_valid) begin
            if (mode_sel_r == 2'b11 && sine_phase_active) begin

                alpha_ref_q16  <= sine_pos_q16;
                dalpha_ref_q16 <= sine_vel_q16;
                ramp_pos_q16   <= sine_pos_q16;
                ramp_vel_q16   <= 32'sd0;
                vel_decay_cnt  <= 2'd0;
            end else begin


                if (ramp_pos_q16 < target_setpoint - RAMP_STEP_Q16) begin
                    ramp_pos_q16  <= ramp_pos_q16 + RAMP_STEP_Q16;
                    ramp_vel_q16  <= RAMP_VEL_Q16;
                    vel_decay_cnt <= 2'd2;
                end else if (ramp_pos_q16 > target_setpoint + RAMP_STEP_Q16) begin
                    ramp_pos_q16  <= ramp_pos_q16 - RAMP_STEP_Q16;
                    ramp_vel_q16  <= -RAMP_VEL_Q16;
                    vel_decay_cnt <= 2'd2;
                end else begin
                    ramp_pos_q16 <= target_setpoint;

                    if (vel_decay_cnt == 2'd2) begin
                        ramp_vel_q16  <= (ramp_vel_q16 >>> 1);
                        vel_decay_cnt <= 2'd1;
                    end else if (vel_decay_cnt == 2'd1) begin
                        ramp_vel_q16  <= (ramp_vel_q16 >>> 1);
                        vel_decay_cnt <= 2'd0;
                    end else begin
                        ramp_vel_q16  <= 32'sd0;
                        vel_decay_cnt <= 2'd0;
                    end
                end
                alpha_ref_q16  <= ramp_pos_q16;
                dalpha_ref_q16 <= ramp_vel_q16;
            end
        end
    end

endmodule
