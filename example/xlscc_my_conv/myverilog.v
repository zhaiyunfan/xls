// a fifo that convert 128bit to 64bit

module
(
    input wire clk,
    input wire rst,
    input wire [127:0] din,
    input wire we,
    output wire [63:0] dout,
    output wire empty,
    output wire full
);

    reg [63:0] fifo [7:0];
    reg [2:0] wr_ptr;
    reg [2:0] rd_ptr;

    assign empty = (wr_ptr == rd_ptr);
    assign full = ((wr_ptr + 1) % 8) == rd_ptr;

    always @(posedge clk or posedge rst)
    begin
        if (rst)
        begin
            wr_ptr <= 3'b0;
            rd_ptr <= 3'b0;
        end
        else
        begin
            if (we && !full)
            begin
                fifo[wr_ptr] <= din[127:64];
                // wr_ptr should add 2 after write
                wr_ptr <= (wr_ptr + 2) % 8;
            end
            if (!empty)
            begin
                dout <= fifo[rd_ptr];
                rd_ptr <= (rd_ptr + 1) % 8;
            end
        end
    end
endmodule