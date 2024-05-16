module xls_test_unroll(
  input wire clk,
  input wire [31:0] x,
  output wire [31:0] out
);
  // lint_off MULTIPLY
  function automatic [31:0] umul32b_32b_x_2b (input reg [31:0] lhs, input reg [1:0] rhs);
    begin
      umul32b_32b_x_2b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [31:0] umul32b_32b_x_3b (input reg [31:0] lhs, input reg [2:0] rhs);
    begin
      umul32b_32b_x_3b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [31:0] umul32b_32b_x_4b (input reg [31:0] lhs, input reg [3:0] rhs);
    begin
      umul32b_32b_x_4b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [31:0] umul32b_32b_x_5b (input reg [31:0] lhs, input reg [4:0] rhs);
    begin
      umul32b_32b_x_5b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [30:0] umul31b_31b_x_2b (input reg [30:0] lhs, input reg [1:0] rhs);
    begin
      umul31b_31b_x_2b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [30:0] umul31b_31b_x_3b (input reg [30:0] lhs, input reg [2:0] rhs);
    begin
      umul31b_31b_x_3b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [29:0] umul30b_30b_x_2b (input reg [29:0] lhs, input reg [1:0] rhs);
    begin
      umul30b_30b_x_2b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [30:0] umul31b_31b_x_4b (input reg [30:0] lhs, input reg [3:0] rhs);
    begin
      umul31b_31b_x_4b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [29:0] umul30b_30b_x_3b (input reg [29:0] lhs, input reg [2:0] rhs);
    begin
      umul30b_30b_x_3b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY
  // lint_off MULTIPLY
  function automatic [28:0] umul29b_29b_x_2b (input reg [28:0] lhs, input reg [1:0] rhs);
    begin
      umul29b_29b_x_2b = lhs * rhs;
    end
  endfunction
  // lint_on MULTIPLY

  // ===== Pipe stage 0:

  // Registers for pipe stage 0:
  reg [31:0] p0_x;
  always_ff @ (posedge clk) begin
    p0_x <= x;
  end

  // ===== Pipe stage 1:

  // Registers for pipe stage 1:
  reg [31:0] p1_x;
  always_ff @ (posedge clk) begin
    p1_x <= p0_x;
  end

  // ===== Pipe stage 2:
  wire [31:0] p2_umul_1767_comb;
  wire [29:0] p2_bit_slice_1768_comb;
  wire [1:0] p2_bit_slice_1769_comb;
  assign p2_umul_1767_comb = umul32b_32b_x_2b(p1_x, 2'h3);
  assign p2_bit_slice_1768_comb = p2_umul_1767_comb[31:2];
  assign p2_bit_slice_1769_comb = p2_umul_1767_comb[1:0];

  // Registers for pipe stage 2:
  reg [31:0] p2_x;
  reg [29:0] p2_bit_slice_1768;
  reg [1:0] p2_bit_slice_1769;
  always_ff @ (posedge clk) begin
    p2_x <= p1_x;
    p2_bit_slice_1768 <= p2_bit_slice_1768_comb;
    p2_bit_slice_1769 <= p2_bit_slice_1769_comb;
  end

  // ===== Pipe stage 3:
  wire [30:0] p3_add_1842_comb;
  wire [29:0] p3_add_1844_comb;
  wire [31:0] p3_umul_1789_comb;
  wire [31:0] p3_umul_1792_comb;
  wire [31:0] p3_umul_1793_comb;
  wire [31:0] p3_umul_1795_comb;
  wire [31:0] p3_umul_1798_comb;
  wire [31:0] p3_umul_1800_comb;
  wire [31:0] p3_umul_1801_comb;
  wire [31:0] p3_umul_1803_comb;
  wire [31:0] p3_umul_1805_comb;
  wire [31:0] p3_umul_1807_comb;
  wire [28:0] p3_bit_slice_1808_comb;
  wire [31:0] p3_umul_1810_comb;
  wire [31:0] p3_umul_1812_comb;
  wire [31:0] p3_umul_1814_comb;
  wire [30:0] p3_bit_slice_1817_comb;
  wire [30:0] p3_umul_1466_NarrowedMult__comb;
  wire [28:0] p3_bit_slice_1819_comb;
  wire [30:0] p3_bit_slice_1820_comb;
  wire [30:0] p3_umul_1472_NarrowedMult__comb;
  wire [29:0] p3_bit_slice_1822_comb;
  wire [29:0] p3_umul_1476_NarrowedMult__comb;
  wire [30:0] p3_bit_slice_1824_comb;
  wire [30:0] p3_umul_1480_NarrowedMult__comb;
  wire [27:0] p3_bit_slice_1826_comb;
  wire [27:0] p3_bit_slice_1827_comb;
  wire [30:0] p3_bit_slice_1828_comb;
  wire [30:0] p3_umul_1486_NarrowedMult__comb;
  wire [29:0] p3_bit_slice_1830_comb;
  wire [29:0] p3_umul_1490_NarrowedMult__comb;
  wire [30:0] p3_bit_slice_1832_comb;
  wire [30:0] p3_umul_1494_NarrowedMult__comb;
  wire [28:0] p3_bit_slice_1834_comb;
  wire [28:0] p3_umul_1498_NarrowedMult__comb;
  wire [30:0] p3_bit_slice_1836_comb;
  wire [30:0] p3_umul_1502_NarrowedMult__comb;
  wire [29:0] p3_bit_slice_1838_comb;
  wire [29:0] p3_umul_1506_NarrowedMult__comb;
  wire [30:0] p3_bit_slice_1840_comb;
  wire [30:0] p3_umul_1510_NarrowedMult__comb;
  wire p3_bit_slice_1845_comb;
  wire [2:0] p3_bit_slice_1846_comb;
  wire p3_bit_slice_1847_comb;
  wire [1:0] p3_bit_slice_1848_comb;
  wire p3_bit_slice_1849_comb;
  wire [3:0] p3_bit_slice_1850_comb;
  wire p3_bit_slice_1851_comb;
  wire [1:0] p3_bit_slice_1852_comb;
  wire p3_bit_slice_1853_comb;
  wire [2:0] p3_bit_slice_1854_comb;
  wire p3_bit_slice_1855_comb;
  wire [1:0] p3_bit_slice_1856_comb;
  wire p3_bit_slice_1857_comb;
  wire [31:0] p3_umul_1861_comb;
  wire [31:0] p3_add_1862_comb;
  assign p3_add_1842_comb = p2_x[31:1] + p2_x[30:0];
  assign p3_add_1844_comb = p2_bit_slice_1768 + p2_x[29:0];
  assign p3_umul_1789_comb = umul32b_32b_x_3b(p2_x, 3'h5);
  assign p3_umul_1792_comb = umul32b_32b_x_3b(p2_x, 3'h7);
  assign p3_umul_1793_comb = umul32b_32b_x_4b(p2_x, 4'h9);
  assign p3_umul_1795_comb = umul32b_32b_x_4b(p2_x, 4'hb);
  assign p3_umul_1798_comb = umul32b_32b_x_4b(p2_x, 4'hd);
  assign p3_umul_1800_comb = umul32b_32b_x_4b(p2_x, 4'hf);
  assign p3_umul_1801_comb = umul32b_32b_x_5b(p2_x, 5'h11);
  assign p3_umul_1803_comb = umul32b_32b_x_5b(p2_x, 5'h13);
  assign p3_umul_1805_comb = umul32b_32b_x_5b(p2_x, 5'h15);
  assign p3_umul_1807_comb = umul32b_32b_x_5b(p2_x, 5'h17);
  assign p3_bit_slice_1808_comb = p2_x[28:0];
  assign p3_umul_1810_comb = umul32b_32b_x_5b(p2_x, 5'h19);
  assign p3_umul_1812_comb = umul32b_32b_x_5b(p2_x, 5'h1b);
  assign p3_umul_1814_comb = umul32b_32b_x_5b(p2_x, 5'h1d);
  assign p3_bit_slice_1817_comb = p3_umul_1789_comb[31:1];
  assign p3_umul_1466_NarrowedMult__comb = umul31b_31b_x_2b(p2_x[30:0], 2'h3);
  assign p3_bit_slice_1819_comb = p3_umul_1792_comb[31:3];
  assign p3_bit_slice_1820_comb = p3_umul_1793_comb[31:1];
  assign p3_umul_1472_NarrowedMult__comb = umul31b_31b_x_3b(p2_x[30:0], 3'h5);
  assign p3_bit_slice_1822_comb = p3_umul_1795_comb[31:2];
  assign p3_umul_1476_NarrowedMult__comb = umul30b_30b_x_2b(p2_x[29:0], 2'h3);
  assign p3_bit_slice_1824_comb = p3_umul_1798_comb[31:1];
  assign p3_umul_1480_NarrowedMult__comb = umul31b_31b_x_3b(p2_x[30:0], 3'h7);
  assign p3_bit_slice_1826_comb = p3_umul_1800_comb[31:4];
  assign p3_bit_slice_1827_comb = p2_x[27:0];
  assign p3_bit_slice_1828_comb = p3_umul_1801_comb[31:1];
  assign p3_umul_1486_NarrowedMult__comb = umul31b_31b_x_4b(p2_x[30:0], 4'h9);
  assign p3_bit_slice_1830_comb = p3_umul_1803_comb[31:2];
  assign p3_umul_1490_NarrowedMult__comb = umul30b_30b_x_3b(p2_x[29:0], 3'h5);
  assign p3_bit_slice_1832_comb = p3_umul_1805_comb[31:1];
  assign p3_umul_1494_NarrowedMult__comb = umul31b_31b_x_4b(p2_x[30:0], 4'hb);
  assign p3_bit_slice_1834_comb = p3_umul_1807_comb[31:3];
  assign p3_umul_1498_NarrowedMult__comb = umul29b_29b_x_2b(p3_bit_slice_1808_comb, 2'h3);
  assign p3_bit_slice_1836_comb = p3_umul_1810_comb[31:1];
  assign p3_umul_1502_NarrowedMult__comb = umul31b_31b_x_4b(p2_x[30:0], 4'hd);
  assign p3_bit_slice_1838_comb = p3_umul_1812_comb[31:2];
  assign p3_umul_1506_NarrowedMult__comb = umul30b_30b_x_3b(p2_x[29:0], 3'h7);
  assign p3_bit_slice_1840_comb = p3_umul_1814_comb[31:1];
  assign p3_umul_1510_NarrowedMult__comb = umul31b_31b_x_4b(p2_x[30:0], 4'hf);
  assign p3_bit_slice_1845_comb = p3_umul_1789_comb[0];
  assign p3_bit_slice_1846_comb = p3_umul_1792_comb[2:0];
  assign p3_bit_slice_1847_comb = p3_umul_1793_comb[0];
  assign p3_bit_slice_1848_comb = p3_umul_1795_comb[1:0];
  assign p3_bit_slice_1849_comb = p3_umul_1798_comb[0];
  assign p3_bit_slice_1850_comb = p3_umul_1800_comb[3:0];
  assign p3_bit_slice_1851_comb = p3_umul_1801_comb[0];
  assign p3_bit_slice_1852_comb = p3_umul_1803_comb[1:0];
  assign p3_bit_slice_1853_comb = p3_umul_1805_comb[0];
  assign p3_bit_slice_1854_comb = p3_umul_1807_comb[2:0];
  assign p3_bit_slice_1855_comb = p3_umul_1810_comb[0];
  assign p3_bit_slice_1856_comb = p3_umul_1812_comb[1:0];
  assign p3_bit_slice_1857_comb = p3_umul_1814_comb[0];
  assign p3_umul_1861_comb = umul32b_32b_x_5b(p2_x, 5'h1f);
  assign p3_add_1862_comb = {p3_add_1842_comb, p2_x[0]} + {p3_add_1844_comb, p2_bit_slice_1769};

  // Registers for pipe stage 3:
  reg [28:0] p3_bit_slice_1808;
  reg [30:0] p3_bit_slice_1817;
  reg [30:0] p3_umul_1466_NarrowedMult_;
  reg [28:0] p3_bit_slice_1819;
  reg [30:0] p3_bit_slice_1820;
  reg [30:0] p3_umul_1472_NarrowedMult_;
  reg [29:0] p3_bit_slice_1822;
  reg [29:0] p3_umul_1476_NarrowedMult_;
  reg [30:0] p3_bit_slice_1824;
  reg [30:0] p3_umul_1480_NarrowedMult_;
  reg [27:0] p3_bit_slice_1826;
  reg [27:0] p3_bit_slice_1827;
  reg [30:0] p3_bit_slice_1828;
  reg [30:0] p3_umul_1486_NarrowedMult_;
  reg [29:0] p3_bit_slice_1830;
  reg [29:0] p3_umul_1490_NarrowedMult_;
  reg [30:0] p3_bit_slice_1832;
  reg [30:0] p3_umul_1494_NarrowedMult_;
  reg [28:0] p3_bit_slice_1834;
  reg [28:0] p3_umul_1498_NarrowedMult_;
  reg [30:0] p3_bit_slice_1836;
  reg [30:0] p3_umul_1502_NarrowedMult_;
  reg [29:0] p3_bit_slice_1838;
  reg [29:0] p3_umul_1506_NarrowedMult_;
  reg [30:0] p3_bit_slice_1840;
  reg [30:0] p3_umul_1510_NarrowedMult_;
  reg p3_bit_slice_1845;
  reg [2:0] p3_bit_slice_1846;
  reg p3_bit_slice_1847;
  reg [1:0] p3_bit_slice_1848;
  reg p3_bit_slice_1849;
  reg [3:0] p3_bit_slice_1850;
  reg p3_bit_slice_1851;
  reg [1:0] p3_bit_slice_1852;
  reg p3_bit_slice_1853;
  reg [2:0] p3_bit_slice_1854;
  reg p3_bit_slice_1855;
  reg [1:0] p3_bit_slice_1856;
  reg p3_bit_slice_1857;
  reg [31:0] p3_umul_1861;
  reg [31:0] p3_add_1862;
  always_ff @ (posedge clk) begin
    p3_bit_slice_1808 <= p3_bit_slice_1808_comb;
    p3_bit_slice_1817 <= p3_bit_slice_1817_comb;
    p3_umul_1466_NarrowedMult_ <= p3_umul_1466_NarrowedMult__comb;
    p3_bit_slice_1819 <= p3_bit_slice_1819_comb;
    p3_bit_slice_1820 <= p3_bit_slice_1820_comb;
    p3_umul_1472_NarrowedMult_ <= p3_umul_1472_NarrowedMult__comb;
    p3_bit_slice_1822 <= p3_bit_slice_1822_comb;
    p3_umul_1476_NarrowedMult_ <= p3_umul_1476_NarrowedMult__comb;
    p3_bit_slice_1824 <= p3_bit_slice_1824_comb;
    p3_umul_1480_NarrowedMult_ <= p3_umul_1480_NarrowedMult__comb;
    p3_bit_slice_1826 <= p3_bit_slice_1826_comb;
    p3_bit_slice_1827 <= p3_bit_slice_1827_comb;
    p3_bit_slice_1828 <= p3_bit_slice_1828_comb;
    p3_umul_1486_NarrowedMult_ <= p3_umul_1486_NarrowedMult__comb;
    p3_bit_slice_1830 <= p3_bit_slice_1830_comb;
    p3_umul_1490_NarrowedMult_ <= p3_umul_1490_NarrowedMult__comb;
    p3_bit_slice_1832 <= p3_bit_slice_1832_comb;
    p3_umul_1494_NarrowedMult_ <= p3_umul_1494_NarrowedMult__comb;
    p3_bit_slice_1834 <= p3_bit_slice_1834_comb;
    p3_umul_1498_NarrowedMult_ <= p3_umul_1498_NarrowedMult__comb;
    p3_bit_slice_1836 <= p3_bit_slice_1836_comb;
    p3_umul_1502_NarrowedMult_ <= p3_umul_1502_NarrowedMult__comb;
    p3_bit_slice_1838 <= p3_bit_slice_1838_comb;
    p3_umul_1506_NarrowedMult_ <= p3_umul_1506_NarrowedMult__comb;
    p3_bit_slice_1840 <= p3_bit_slice_1840_comb;
    p3_umul_1510_NarrowedMult_ <= p3_umul_1510_NarrowedMult__comb;
    p3_bit_slice_1845 <= p3_bit_slice_1845_comb;
    p3_bit_slice_1846 <= p3_bit_slice_1846_comb;
    p3_bit_slice_1847 <= p3_bit_slice_1847_comb;
    p3_bit_slice_1848 <= p3_bit_slice_1848_comb;
    p3_bit_slice_1849 <= p3_bit_slice_1849_comb;
    p3_bit_slice_1850 <= p3_bit_slice_1850_comb;
    p3_bit_slice_1851 <= p3_bit_slice_1851_comb;
    p3_bit_slice_1852 <= p3_bit_slice_1852_comb;
    p3_bit_slice_1853 <= p3_bit_slice_1853_comb;
    p3_bit_slice_1854 <= p3_bit_slice_1854_comb;
    p3_bit_slice_1855 <= p3_bit_slice_1855_comb;
    p3_bit_slice_1856 <= p3_bit_slice_1856_comb;
    p3_bit_slice_1857 <= p3_bit_slice_1857_comb;
    p3_umul_1861 <= p3_umul_1861_comb;
    p3_add_1862 <= p3_add_1862_comb;
  end

  // ===== Pipe stage 4:
  wire [30:0] p4_add_1945_comb;
  wire [28:0] p4_add_1946_comb;
  wire [30:0] p4_add_1947_comb;
  wire [29:0] p4_add_1948_comb;
  wire [30:0] p4_add_1949_comb;
  wire [27:0] p4_add_1950_comb;
  wire [30:0] p4_add_1951_comb;
  wire [29:0] p4_add_1952_comb;
  wire [30:0] p4_add_1953_comb;
  wire [28:0] p4_add_1954_comb;
  wire [30:0] p4_add_1955_comb;
  wire [29:0] p4_add_1956_comb;
  wire [30:0] p4_add_1957_comb;
  wire [31:0] p4_add_1971_comb;
  wire [31:0] p4_add_1972_comb;
  wire [31:0] p4_add_1973_comb;
  wire [31:0] p4_add_1974_comb;
  wire [31:0] p4_add_1975_comb;
  wire [31:0] p4_add_1976_comb;
  wire [31:0] p4_add_1977_comb;
  wire [31:0] p4_add_1978_comb;
  wire [31:0] p4_add_1979_comb;
  wire [31:0] p4_add_1980_comb;
  wire [31:0] p4_add_1981_comb;
  assign p4_add_1945_comb = p3_bit_slice_1817 + p3_umul_1466_NarrowedMult_;
  assign p4_add_1946_comb = p3_bit_slice_1819 + p3_bit_slice_1808;
  assign p4_add_1947_comb = p3_bit_slice_1820 + p3_umul_1472_NarrowedMult_;
  assign p4_add_1948_comb = p3_bit_slice_1822 + p3_umul_1476_NarrowedMult_;
  assign p4_add_1949_comb = p3_bit_slice_1824 + p3_umul_1480_NarrowedMult_;
  assign p4_add_1950_comb = p3_bit_slice_1826 + p3_bit_slice_1827;
  assign p4_add_1951_comb = p3_bit_slice_1828 + p3_umul_1486_NarrowedMult_;
  assign p4_add_1952_comb = p3_bit_slice_1830 + p3_umul_1490_NarrowedMult_;
  assign p4_add_1953_comb = p3_bit_slice_1832 + p3_umul_1494_NarrowedMult_;
  assign p4_add_1954_comb = p3_bit_slice_1834 + p3_umul_1498_NarrowedMult_;
  assign p4_add_1955_comb = p3_bit_slice_1836 + p3_umul_1502_NarrowedMult_;
  assign p4_add_1956_comb = p3_bit_slice_1838 + p3_umul_1506_NarrowedMult_;
  assign p4_add_1957_comb = p3_bit_slice_1840 + p3_umul_1510_NarrowedMult_;
  assign p4_add_1971_comb = {p4_add_1945_comb, p3_bit_slice_1845} + {p4_add_1946_comb, p3_bit_slice_1846};
  assign p4_add_1972_comb = {p4_add_1947_comb, p3_bit_slice_1847} + {p4_add_1948_comb, p3_bit_slice_1848};
  assign p4_add_1973_comb = {p4_add_1949_comb, p3_bit_slice_1849} + {p4_add_1950_comb, p3_bit_slice_1850};
  assign p4_add_1974_comb = {p4_add_1951_comb, p3_bit_slice_1851} + {p4_add_1952_comb, p3_bit_slice_1852};
  assign p4_add_1975_comb = {p4_add_1953_comb, p3_bit_slice_1853} + {p4_add_1954_comb, p3_bit_slice_1854};
  assign p4_add_1976_comb = {p4_add_1955_comb, p3_bit_slice_1855} + {p4_add_1956_comb, p3_bit_slice_1856};
  assign p4_add_1977_comb = {p4_add_1957_comb, p3_bit_slice_1857} + p3_umul_1861;
  assign p4_add_1978_comb = p3_add_1862 + p4_add_1971_comb;
  assign p4_add_1979_comb = p4_add_1972_comb + p4_add_1973_comb;
  assign p4_add_1980_comb = p4_add_1974_comb + p4_add_1975_comb;
  assign p4_add_1981_comb = p4_add_1976_comb + p4_add_1977_comb;

  // Registers for pipe stage 4:
  reg [31:0] p4_add_1978;
  reg [31:0] p4_add_1979;
  reg [31:0] p4_add_1980;
  reg [31:0] p4_add_1981;
  always_ff @ (posedge clk) begin
    p4_add_1978 <= p4_add_1978_comb;
    p4_add_1979 <= p4_add_1979_comb;
    p4_add_1980 <= p4_add_1980_comb;
    p4_add_1981 <= p4_add_1981_comb;
  end

  // ===== Pipe stage 5:
  wire [31:0] p5_add_1990_comb;
  wire [31:0] p5_add_1991_comb;
  wire [31:0] p5_add_1992_comb;
  assign p5_add_1990_comb = p4_add_1978 + p4_add_1979;
  assign p5_add_1991_comb = p4_add_1980 + p4_add_1981;
  assign p5_add_1992_comb = p5_add_1990_comb + p5_add_1991_comb;

  // Registers for pipe stage 5:
  reg [31:0] p5_add_1992;
  always_ff @ (posedge clk) begin
    p5_add_1992 <= p5_add_1992_comb;
  end
  assign out = p5_add_1992;
endmodule
