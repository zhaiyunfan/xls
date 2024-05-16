
void convolve(const short signal[16], const short kernel[16],
              short output[16]) {
  const short signalSize = 16;
  const short kernelSize = 16;
  const short outputSize = 16;
#pragma hls_unroll yes
  for (short i = 0; i < outputSize; ++i) {
    output[i] = 0;
  }
#pragma hls_unroll yes
  for (short i = 0; i < outputSize; ++i) {
#pragma hls_unroll yes
    for (short j = 0; j < kernelSize; ++j) {
      if (i - j >= 0 && i - j < signalSize) {
        output[i] += signal[i - j] * kernel[j];
      }
    }
  }
}

short exp_approx(short x) {
  short sum = 1.0;
  short term = 1.0;
#pragma hls_unroll yes
  for (int i = 1; i < 20; ++i) {
    term *= x / i;
    sum += term;
  }
  return sum;
}

void sigmoid(short input[16], short output[16]) {
#pragma hls_unroll yes
  for (int i = 0; i < 16; ++i) {
    output[i] = 1 / (1 + exp_approx(-input[i]));
  }
}

#pragma hls_top
void conv_top(const short signal[16], const short kernel[16],
              short output[16]) {
  short temp_output[16];

  convolve(signal, kernel, temp_output);

  sigmoid(temp_output, output);
}