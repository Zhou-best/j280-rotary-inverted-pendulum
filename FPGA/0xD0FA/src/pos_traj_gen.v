module pos_traj_gen #(
    parameter integer VMAX_Q16   = 58982,


    parameter integer VSOFT_Q16  = 6554,
    parameter integer RAMP_Q16   = 12,
    parameter integer ASTEP_Q16  = 328,
    parameter integer SINE_INC   = 839
)(
    input  wire               clk,
    input  wire               rst_n,
    input  wire               calc_en,
    input  wire               sync,
    input  wire               rezero,
    input  wire               enable,
    input  wire [2:0]         mode_sel,
    input  wire signed [31:0] alpha_meas_q16,

    output reg  signed [31:0] alpha_ref_q16,
    output reg  signed [31:0] dalpha_ref_q16,
    output wire               moving
);


    localparam signed [31:0] POS_45_Q16 = 32'sd51472;
    localparam signed [31:0] POS_90_Q16 = 32'sd102944;


    reg  [6:0]         sine_idx;
    reg  signed [15:0] sin_q14;

    reg signed [31:0]  pos_q16;
    reg  [22:0]        phase_acc;


    localparam [22:0]  SINE_INC_23 = SINE_INC;

    always @(*) begin
        sine_idx = phase_acc[22:16];
    end

    always @(*) begin
        case (sine_idx)
            7'd0:   sin_q14 = 16'sd0;      7'd1:   sin_q14 = 16'sd804;
            7'd2:   sin_q14 = 16'sd1606;   7'd3:   sin_q14 = 16'sd2404;
            7'd4:   sin_q14 = 16'sd3196;   7'd5:   sin_q14 = 16'sd3981;
            7'd6:   sin_q14 = 16'sd4756;   7'd7:   sin_q14 = 16'sd5520;
            7'd8:   sin_q14 = 16'sd6270;   7'd9:   sin_q14 = 16'sd7005;
            7'd10:  sin_q14 = 16'sd7723;   7'd11:  sin_q14 = 16'sd8423;
            7'd12:  sin_q14 = 16'sd9102;   7'd13:  sin_q14 = 16'sd9760;
            7'd14:  sin_q14 = 16'sd10394;  7'd15:  sin_q14 = 16'sd11003;
            7'd16:  sin_q14 = 16'sd11585;  7'd17:  sin_q14 = 16'sd12140;
            7'd18:  sin_q14 = 16'sd12665;  7'd19:  sin_q14 = 16'sd13160;
            7'd20:  sin_q14 = 16'sd13623;  7'd21:  sin_q14 = 16'sd14053;
            7'd22:  sin_q14 = 16'sd14449;  7'd23:  sin_q14 = 16'sd14811;
            7'd24:  sin_q14 = 16'sd15137;  7'd25:  sin_q14 = 16'sd15426;
            7'd26:  sin_q14 = 16'sd15679;  7'd27:  sin_q14 = 16'sd15893;
            7'd28:  sin_q14 = 16'sd16069;  7'd29:  sin_q14 = 16'sd16207;
            7'd30:  sin_q14 = 16'sd16305;  7'd31:  sin_q14 = 16'sd16364;
            7'd32:  sin_q14 = 16'sd16384;  7'd33:  sin_q14 = 16'sd16364;
            7'd34:  sin_q14 = 16'sd16305;  7'd35:  sin_q14 = 16'sd16207;
            7'd36:  sin_q14 = 16'sd16069;  7'd37:  sin_q14 = 16'sd15893;
            7'd38:  sin_q14 = 16'sd15679;  7'd39:  sin_q14 = 16'sd15426;
            7'd40:  sin_q14 = 16'sd15137;  7'd41:  sin_q14 = 16'sd14811;
            7'd42:  sin_q14 = 16'sd14449;  7'd43:  sin_q14 = 16'sd14053;
            7'd44:  sin_q14 = 16'sd13623;  7'd45:  sin_q14 = 16'sd13160;
            7'd46:  sin_q14 = 16'sd12665;  7'd47:  sin_q14 = 16'sd12140;
            7'd48:  sin_q14 = 16'sd11585;  7'd49:  sin_q14 = 16'sd11003;
            7'd50:  sin_q14 = 16'sd10394;  7'd51:  sin_q14 = 16'sd9760;
            7'd52:  sin_q14 = 16'sd9102;   7'd53:  sin_q14 = 16'sd8423;
            7'd54:  sin_q14 = 16'sd7723;   7'd55:  sin_q14 = 16'sd7005;
            7'd56:  sin_q14 = 16'sd6270;   7'd57:  sin_q14 = 16'sd5520;
            7'd58:  sin_q14 = 16'sd4756;   7'd59:  sin_q14 = 16'sd3981;
            7'd60:  sin_q14 = 16'sd3196;   7'd61:  sin_q14 = 16'sd2404;
            7'd62:  sin_q14 = 16'sd1606;   7'd63:  sin_q14 = 16'sd804;
            7'd64:  sin_q14 = 16'sd0;      7'd65:  sin_q14 = -16'sd804;
            7'd66:  sin_q14 = -16'sd1606;  7'd67:  sin_q14 = -16'sd2404;
            7'd68:  sin_q14 = -16'sd3196;  7'd69:  sin_q14 = -16'sd3981;
            7'd70:  sin_q14 = -16'sd4756;  7'd71:  sin_q14 = -16'sd5520;
            7'd72:  sin_q14 = -16'sd6270;  7'd73:  sin_q14 = -16'sd7005;
            7'd74:  sin_q14 = -16'sd7723;  7'd75:  sin_q14 = -16'sd8423;
            7'd76:  sin_q14 = -16'sd9102;  7'd77:  sin_q14 = -16'sd9760;
            7'd78:  sin_q14 = -16'sd10394; 7'd79:  sin_q14 = -16'sd11003;
            7'd80:  sin_q14 = -16'sd11585; 7'd81:  sin_q14 = -16'sd12140;
            7'd82:  sin_q14 = -16'sd12665; 7'd83:  sin_q14 = -16'sd13160;
            7'd84:  sin_q14 = -16'sd13623; 7'd85:  sin_q14 = -16'sd14053;
            7'd86:  sin_q14 = -16'sd14449; 7'd87:  sin_q14 = -16'sd14811;
            7'd88:  sin_q14 = -16'sd15137; 7'd89:  sin_q14 = -16'sd15426;
            7'd90:  sin_q14 = -16'sd15679; 7'd91:  sin_q14 = -16'sd15893;
            7'd92:  sin_q14 = -16'sd16069; 7'd93:  sin_q14 = -16'sd16207;
            7'd94:  sin_q14 = -16'sd16305; 7'd95:  sin_q14 = -16'sd16364;
            7'd96:  sin_q14 = -16'sd16384; 7'd97:  sin_q14 = -16'sd16364;
            7'd98:  sin_q14 = -16'sd16305; 7'd99:  sin_q14 = -16'sd16207;
            7'd100: sin_q14 = -16'sd16069; 7'd101: sin_q14 = -16'sd15893;
            7'd102: sin_q14 = -16'sd15679; 7'd103: sin_q14 = -16'sd15426;
            7'd104: sin_q14 = -16'sd15137; 7'd105: sin_q14 = -16'sd14811;
            7'd106: sin_q14 = -16'sd14449; 7'd107: sin_q14 = -16'sd14053;
            7'd108: sin_q14 = -16'sd13623; 7'd109: sin_q14 = -16'sd13160;
            7'd110: sin_q14 = -16'sd12665; 7'd111: sin_q14 = -16'sd12140;
            7'd112: sin_q14 = -16'sd11585; 7'd113: sin_q14 = -16'sd11003;
            7'd114: sin_q14 = -16'sd10394; 7'd115: sin_q14 = -16'sd9760;
            7'd116: sin_q14 = -16'sd9102;  7'd117: sin_q14 = -16'sd8423;
            7'd118: sin_q14 = -16'sd7723;  7'd119: sin_q14 = -16'sd7005;
            7'd120: sin_q14 = -16'sd6270;  7'd121: sin_q14 = -16'sd5520;
            7'd122: sin_q14 = -16'sd4756;  7'd123: sin_q14 = -16'sd3981;
            7'd124: sin_q14 = -16'sd3196;  7'd125: sin_q14 = -16'sd2404;
            7'd126: sin_q14 = -16'sd1606;  7'd127: sin_q14 = -16'sd804;
            default:sin_q14 = 16'sd0;
        endcase
    end


    wire signed [31:0] sin_ext = {{16{sin_q14[15]}}, sin_q14};
    wire signed [31:0] sine_pos_q16 = (sin_ext <<< 1) + (sin_ext >>> 2)
                                    + (sin_ext >>> 3) + (sin_ext >>> 5);


    reg signed [31:0] fixed_target_q16;
    always @(*) begin
        case (mode_sel)
            3'd1:    fixed_target_q16 = POS_45_Q16;
            3'd2:    fixed_target_q16 = -POS_45_Q16;
            3'd3:    fixed_target_q16 = POS_90_Q16;
            3'd4:    fixed_target_q16 = -POS_90_Q16;
            default: fixed_target_q16 = 32'sd0;
        endcase
    end


    reg signed [31:0] vel_q16;
    reg signed [31:0] vlim_q16;


    reg signed [31:0] sin_pos_s1;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sin_pos_s1 <= 32'sd0;
            phase_acc  <= 23'd0;
        end else if (sync || rezero) begin
            sin_pos_s1 <= 32'sd0;
            phase_acc  <= 23'd0;
        end else if (calc_en && enable) begin
            sin_pos_s1 <= sine_pos_q16;


            if (mode_sel == 3'd5) phase_acc <= phase_acc + SINE_INC_23;
            else                  phase_acc <= 23'd0;
        end
    end


    reg signed [31:0] target_s2, dist_s2, vlim_s2;

    wire signed [31:0] target_now = (mode_sel == 3'd5) ? sin_pos_s1 : fixed_target_q16;
    wire signed [31:0] dist_now   = target_now - pos_q16;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            target_s2 <= 32'sd0; dist_s2 <= 32'sd0; vlim_s2 <= VSOFT_Q16;
        end else if (sync || rezero) begin

            target_s2 <= 32'sd0; dist_s2 <= 32'sd0; vlim_s2 <= VSOFT_Q16;
        end else if (calc_en && enable) begin
            target_s2 <= target_now;
            dist_s2   <= dist_now;
            vlim_s2   <= vlim_q16;
        end
    end


    reg signed [31:0] vcmd_s2;
    wire signed [31:0] vcmd_b = (mode_sel == 3'd5)
                              ? ((dist_s2 <<< 2) + (dist_s2 >>> 1) + (dist_s2 >>> 4))
                              : ((dist_s2 >>> 1) + (dist_s2 >>> 3));

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)              vcmd_s2 <= 32'sd0;
        else if (sync || rezero) vcmd_s2 <= 32'sd0;
        else if (calc_en && enable) vcmd_s2 <= vcmd_b;
    end


    reg signed [31:0] vlimd_s3, dist_s3;

    wire signed [31:0] v_limited = (vcmd_s2 >  vlim_s2) ?  vlim_s2 :
                                   (vcmd_s2 < -vlim_s2) ? -vlim_s2 : vcmd_s2;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            vlimd_s3 <= 32'sd0;
            dist_s3  <= 32'sd0;
        end else if (sync || rezero) begin
            vlimd_s3 <= 32'sd0;
            dist_s3  <= 32'sd0;
        end else if (calc_en && enable) begin
            vlimd_s3 <= v_limited;
            dist_s3  <= dist_s2;
        end
    end


    wire signed [31:0] dv = vlimd_s3 - vel_q16;
    wire signed [31:0] vel_next = (dv >  ASTEP_Q16) ? (vel_q16 + ASTEP_Q16) :
                                  (dv < -ASTEP_Q16) ? (vel_q16 - ASTEP_Q16) :
                                  vlimd_s3;


    wire signed [31:0] step_raw = vel_next >>> 10;
    wire signed [31:0] step_now = (step_raw == 32'sd0 && vel_next > 32'sd0) ? 32'sd1 : step_raw;
    wire signed [31:0] pos_next = pos_q16 + step_now;


    reg [2:0] mode_d;
    reg [2:0] settle_hold;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mode_d      <= 3'd0;
            settle_hold <= 3'd0;
        end else if (sync) begin


            mode_d      <= mode_sel;
            settle_hold <= 3'd4;
        end else if (rezero) begin
            mode_d      <= mode_sel;
            settle_hold <= 3'd0;
        end else if (calc_en && enable) begin
            mode_d <= mode_sel;
            if (mode_sel != mode_d)
                settle_hold <= 3'd4;
            else if (settle_hold != 3'd0)
                settle_hold <= settle_hold - 1'b1;
        end
    end


    localparam signed [31:0] SETTLE_Q16 = 32'sd512;
    wire settled = (settle_hold == 3'd0) && (mode_sel != 3'd5) &&
                   (dist_s3 < SETTLE_Q16)  && (dist_s3 > -SETTLE_Q16) &&
                   (vel_next < SETTLE_Q16) && (vel_next > -SETTLE_Q16);


    assign moving = (dist_s2 > 32'sd655) || (dist_s2 < -32'sd655) ||
                    (vel_q16 > 32'sd655) || (vel_q16 < -32'sd655);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pos_q16        <= 32'sd0;
            vel_q16        <= 32'sd0;
            vlim_q16       <= VSOFT_Q16;
            alpha_ref_q16  <= 32'sd0;
            dalpha_ref_q16 <= 32'sd0;
        end else if (sync) begin

            pos_q16        <= alpha_meas_q16;
            vel_q16        <= 32'sd0;
            vlim_q16       <= VSOFT_Q16;
            alpha_ref_q16  <= alpha_meas_q16;
            dalpha_ref_q16 <= 32'sd0;
        end else if (rezero) begin


            pos_q16        <= 32'sd0;
            vel_q16        <= 32'sd0;
            vlim_q16       <= VSOFT_Q16;
            alpha_ref_q16  <= 32'sd0;
            dalpha_ref_q16 <= 32'sd0;
        end else if (calc_en && enable) begin

            if (vlim_q16 < VMAX_Q16) begin
                if (vlim_q16 + RAMP_Q16 > VMAX_Q16)
                    vlim_q16 <= VMAX_Q16;
                else
                    vlim_q16 <= vlim_q16 + RAMP_Q16;
            end

            if (settled) begin
                pos_q16        <= target_s2;
                vel_q16        <= 32'sd0;
                alpha_ref_q16  <= target_s2;
                dalpha_ref_q16 <= 32'sd0;
            end else begin
                pos_q16        <= pos_next;
                vel_q16        <= vel_next;
                alpha_ref_q16  <= pos_next;


                dalpha_ref_q16 <= 32'sd0;
            end
        end
    end

endmodule
