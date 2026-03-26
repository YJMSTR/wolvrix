module graph_assembly_register_init(
    output logic [31:0] out,
    output logic [31:0] out_urandom,
    output logic [127:0] out_wide,
    output logic [3:0] out_x,
    output logic conflict_out
);
    logic [31:0] random_bits = $random;
    logic [31:0] urandom_bits = $urandom;
    logic [127:0] wide_init = 128'h0123456789abcdef0011223344556677;
    logic [3:0] x_init = 4'bx;
    logic conflict_reg = 1'b0;

    initial conflict_reg = 1'b1;

    always_comb begin
        out = random_bits;
        out_urandom = urandom_bits;
        out_wide = wide_init;
        out_x = x_init;
        conflict_out = conflict_reg;
    end
endmodule
