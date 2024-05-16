#!/bin/bash

cd ../../

./bazel-bin/xls/contrib/xlscc/xlscc  ./example/xlscc_my_conv/conv.cc > ./example/xlscc_my_conv/conv.ir &&
./bazel-bin/xls/tools/opt_main  ./example/xlscc_my_conv/conv.ir > ./example/xlscc_my_conv/conv.opt.ir &&
./bazel-bin/xls/tools/codegen_main  ./example/xlscc_my_conv/conv.opt.ir \
  --generator=pipeline \      
  --delay_model="unit" \
  --output_verilog_path=./example/xlscc_my_conv/conv.v \
  --module_name=xls_conv_top \
  --top=conv_top \
  --pipeline_stages=5 \