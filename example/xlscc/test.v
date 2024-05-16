module xls_test(
  input wire [31:0] input,
  output wire [31:0] out
);
  wire [31:0] add_6;
  assign add_6 = input + 32'h0000_0003;
  assign out = add_6;
endmodule
