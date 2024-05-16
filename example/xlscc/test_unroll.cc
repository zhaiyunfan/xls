#pragma hls_top
int test_unroll(int x) {
  int ret = 0;
#pragma hls_unroll yes
  for (int i = 0; i < 32; i++) {
    ret += x * i;
  }
  return ret;
}