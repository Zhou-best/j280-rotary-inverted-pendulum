module ctrl_fsm (
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   calc_en,
    input  wire                   motor_en_sw,
    input  wire                   calib_done,
    input  wire                   soft_limit_err,
    input  wire signed [31:0]     theta_err_q16,
    input  wire signed [31:0]     dtheta_q16,
    input  wire signed [31:0]     dalpha_q16,

    input  wire signed [15:0]     pwm_swing_duty,
    input  wire signed [15:0]     pwm_lqr_duty,
    input  wire                   lqr_calc_done,
    input  wire                   swing_calc_done,

    output reg  [1:0]             current_state,
    output reg  signed [15:0]     final_pwm_duty,
    output reg                    reset_integral_pulse,
    output reg                    traj_sync_pulse,
    output wire                   is_in_balance_zone,
    output wire                   lqr_en,
    output wire                   swing_en,
    output wire                   motor_active
);


    localparam [1:0] STATE_HANGING = 2'd0;
    localparam [1:0] STATE_SWINGUP = 2'd1;
    localparam [1:0] STATE_BALANCE = 2'd2;
    localparam [1:0] STATE_PROTECT = 2'd3;


    localparam signed [31:0] BAL_ANG_THRESH = 32'sd25166;


    localparam signed [31:0] BAL_OMG_THRESH = 32'sd262144;
    localparam signed [31:0] FALL_THRESH    = 32'sd51472;


    localparam signed [31:0] BAL_DALPHA_THRESH = 32'sd98304;


    wire signed [31:0] abs_th  = (theta_err_q16 < 0) ? (-theta_err_q16) : theta_err_q16;
    wire signed [31:0] abs_dth = (dtheta_q16 < 0)    ? (-dtheta_q16)    : dtheta_q16;
    wire signed [31:0] abs_da  = (dalpha_q16 < 0)    ? (-dalpha_q16)    : dalpha_q16;


    wire can_enter_balance = (abs_th < BAL_ANG_THRESH) && (abs_dth < BAL_OMG_THRESH) &&
                             (abs_da < BAL_DALPHA_THRESH);
    assign is_in_balance_zone = (abs_th < BAL_ANG_THRESH);


    wire is_fall_down = (abs_th > FALL_THRESH);


    assign lqr_en       = (current_state == STATE_BALANCE);
    assign swing_en     = (current_state == STATE_SWINGUP);
    assign motor_active = motor_en_sw && (current_state != STATE_HANGING) && (current_state != STATE_PROTECT);

    reg init_kick_active;


    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            current_state        <= STATE_HANGING;
            final_pwm_duty       <= 16'sd0;
            reset_integral_pulse <= 1'b0;
            traj_sync_pulse      <= 1'b0;
            init_kick_active     <= 1'b0;
        end else if (!motor_en_sw) begin

            current_state        <= STATE_HANGING;
            final_pwm_duty       <= 16'sd0;
            reset_integral_pulse <= 1'b0;
            traj_sync_pulse      <= 1'b0;
            init_kick_active     <= 1'b0;
        end else if (soft_limit_err) begin

            current_state        <= STATE_PROTECT;
            final_pwm_duty       <= 16'sd0;
            reset_integral_pulse <= 1'b0;
            traj_sync_pulse      <= 1'b0;
            init_kick_active     <= 1'b0;
        end else if (calc_en) begin
            case (current_state)
                STATE_HANGING: begin
                    reset_integral_pulse <= 1'b0;
                    traj_sync_pulse      <= 1'b0;
                    if (motor_en_sw && calib_done) begin

                        current_state    <= STATE_SWINGUP;
                        final_pwm_duty   <= 16'sd250;
                        init_kick_active <= 1'b1;
                    end else begin
                        final_pwm_duty   <= 16'sd0;
                        init_kick_active <= 1'b0;
                    end
                end

                STATE_SWINGUP: begin
                    init_kick_active <= 1'b0;
                    if (can_enter_balance) begin

                        current_state        <= STATE_BALANCE;
                        final_pwm_duty       <= pwm_lqr_duty;
                        reset_integral_pulse <= 1'b1;
                        traj_sync_pulse      <= 1'b1;
                    end else begin

                        current_state        <= STATE_SWINGUP;
                        reset_integral_pulse <= 1'b0;
                        traj_sync_pulse      <= 1'b0;
                    end
                end

                STATE_BALANCE: begin
                    reset_integral_pulse <= 1'b0;
                    traj_sync_pulse      <= 1'b0;
                    if (is_fall_down) begin

                        current_state  <= STATE_SWINGUP;
                        final_pwm_duty <= 16'sd0;
                    end else begin

                        current_state  <= STATE_BALANCE;
                    end
                end

                STATE_PROTECT: begin

                    if (!soft_limit_err) begin
                        current_state <= STATE_HANGING;
                    end
                    final_pwm_duty       <= 16'sd0;
                    reset_integral_pulse <= 1'b0;
                    traj_sync_pulse      <= 1'b0;
                    init_kick_active     <= 1'b0;
                end

                default: begin
                    current_state        <= STATE_HANGING;
                    final_pwm_duty       <= 16'sd0;
                    reset_integral_pulse <= 1'b0;
                    traj_sync_pulse      <= 1'b0;
                    init_kick_active     <= 1'b0;
                end
            endcase
        end else begin
            reset_integral_pulse <= 1'b0;
            traj_sync_pulse      <= 1'b0;

            if (lqr_calc_done && (current_state == STATE_BALANCE) && !is_fall_down) begin
                final_pwm_duty <= pwm_lqr_duty;
            end else if (swing_calc_done && (current_state == STATE_SWINGUP) && !init_kick_active) begin
                final_pwm_duty <= pwm_swing_duty;
            end
        end
    end

endmodule
