#
# Copyright 2020 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import stat
import subprocess

from absl.testing import absltest
from xls.common import runfiles

IR_MINIMIZER_MAIN_PATH = runfiles.get_path('xls/tools/ir_minimizer_main')

ADD_IR = """package foo

top fn foo(x: bits[32], y: bits[32]) -> bits[32] {
  not.1: bits[32] = not(x, id=1)
  add.2: bits[32] = add(not.1, y, id=2)
  ret not.3: bits[32] = not(add.2, id=3)
}
"""

ARRAY_IR = """package foo

fn bar(x: bits[8][8]) -> bits[8][4] {
  literal.2: bits[32] = literal(value=0, id=2)
  literal.4: bits[32] = literal(value=1, id=4)
  literal.7: bits[32] = literal(value=2, id=7)
  literal.9: bits[32] = literal(value=3, id=9)
  literal.12: bits[32] = literal(value=4, id=12)
  literal.14: bits[32] = literal(value=5, id=14)
  literal.17: bits[32] = literal(value=6, id=17)
  literal.19: bits[32] = literal(value=7, id=19)
  array_index.3: bits[8] = array_index(x, indices=[literal.2], id=3)
  array_index.5: bits[8] = array_index(x, indices=[literal.4], id=5)
  array_index.8: bits[8] = array_index(x, indices=[literal.7], id=8)
  array_index.10: bits[8] = array_index(x, indices=[literal.9], id=10)
  array_index.13: bits[8] = array_index(x, indices=[literal.12], id=13)
  array_index.15: bits[8] = array_index(x, indices=[literal.14], id=15)
  array_index.18: bits[8] = array_index(x, indices=[literal.17], id=18)
  array_index.20: bits[8] = array_index(x, indices=[literal.19], id=20)
  add.6: bits[8] = add(array_index.3, array_index.5, id=6)
  add.11: bits[8] = add(array_index.8, array_index.10, id=11)
  add.16: bits[8] = add(array_index.13, array_index.15, id=16)
  add.21: bits[8] = add(array_index.18, array_index.20, id=21)
  ret array.22: bits[8][4] = array(add.6, add.11, add.16, add.21, id=22)
}

top fn foo(x: bits[8], y: bits[8]) -> bits[8][4] {
  array.25: bits[8][8] = array(x, y, x, y, x, y, x, y, id=25)
  ret invoke.26: bits[8][4] = invoke(array.25, to_apply=bar, id=26)
}
"""

INVOKE_TWO = """package foo

fn bar(x: bits[32]) -> bits[1] {
    ret and_reduce.1: bits[1] = and_reduce(x, id=1)
}

fn baz(x: bits[32]) -> bits[1] {
    ret or_reduce.3: bits[1] = or_reduce(x, id=3)
}

top fn foo(x: bits[32], y: bits[32]) -> bits[1] {
  invoke.6: bits[1] = invoke(x, to_apply=bar, id=6)
  invoke.7: bits[1] = invoke(y, to_apply=baz, id=7)
  ret and.8: bits[1] = and(invoke.6, invoke.7, id=8)
}
"""

INVOKE_TWO_DEEP = """package foo

fn mul(a: bits[8], b: bits[8]) -> bits[8] {
  ret umul.3: bits[8] = umul(a, b, id=3)
}

fn muladd(a: bits[8], b: bits[8], c: bits[8]) -> bits[8] {
  invoke.7: bits[8] = invoke(a, b, to_apply=mul, id=7)
  ret add.8: bits[8] = add(invoke.7, c, id=8)
}

top fn muladdadd(a: bits[8], b: bits[8], c: bits[8], d: bits[8]) -> bits[8] {
  invoke.13: bits[8] = invoke(a, b, c, to_apply=muladd, id=13)
  ret add.14: bits[8] = add(invoke.13, d, id=14)
}
"""

INVOKE_MAP = """package foo

fn bar(x: bits[32]) -> bits[1] {
    ret and_reduce.1: bits[1] = and_reduce(x, id=1)
}

top fn foo(x: bits[32][8]) -> bits[1][8] {
    ret map.3: bits[1][8] = map(x, to_apply=bar, id=3)
}
"""

PROC = '''package testit

file_number 0 "/tmp/testit.x"

chan testit__output(bits[32], id=0, kind=streaming, ops=send_only, flow_control=ready_valid, strictness=proven_mutually_exclusive, metadata="""""")

top proc __testit__main_0_next(__state: bits[32], init={3}) {
  __token: token = literal(value=token, id=1)
  literal.4: bits[32] = literal(value=3, id=4, pos=[(0,9,46)])
  umul.5: bits[32] = umul(__state, literal.4, id=5, pos=[(0,9,40)])
  literal.7: bits[32] = literal(value=1, id=7, pos=[(0,10,18)])
  tok: token = send(__token, umul.5, channel=testit__output, id=6)
  literal.3: bits[1] = literal(value=1, id=3)
  add.8: bits[32] = add(__state, literal.7, id=8, pos=[(0,10,12)])
  next (add.8)
}
'''


class IrMinimizerMainTest(absltest.TestCase):

  def _maybe_record_property(self, name, value):
    if callable(getattr(self, 'recordProperty', None)):
      self.recordProperty(name, value)

  def _write_sh_script(self, path, commands):
    with open(path, 'w') as f:
      all_cmds = ['#!/bin/sh -e'] + commands
      self._maybe_record_property('test_script', '\n'.join(all_cmds))
      f.write('\n'.join(all_cmds))
    st = os.stat(path)
    os.chmod(path, st.st_mode | stat.S_IXUSR)

  def test_minimize_extract_things(self):
    ir_file = self.create_tempfile(content=PROC)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(
        test_sh_file.full_path, ["/usr/bin/env grep 'add' $1"]
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params=true',
        '--can_remove_sends=true',
        '--can_remove_receives=true',
        '--can_extract_segments=true',
        ir_file.full_path,
    ])
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package testit

top fn __testit__main_0_next() -> bits[32] {
  literal.1: bits[32] = literal(value=0, id=1)
  ret add.3: bits[32] = add(literal.1, literal.1, id=3, pos=[(0,10,12)])
}
""",
    )

  def test_minimize_inline_one_can_inline_other_invokes(self):
    ir_file = self.create_tempfile(content=INVOKE_TWO_DEEP)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(
        test_sh_file.full_path, ["/usr/bin/env grep 'invoke.*to_apply=mul,' $1"]
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params=false',
        ir_file.full_path,
    ])
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

fn mul(a: bits[8], b: bits[8]) -> bits[8] {
  ret literal.21: bits[8] = literal(value=0, id=21)
}

top fn muladdadd(a: bits[8], b: bits[8], c: bits[8], d: bits[8]) -> bits[8] {
  literal.55: bits[8] = literal(value=0, id=55)
  ret invoke.45: bits[8] = invoke(literal.55, literal.55, to_apply=mul, id=45)
}
""",
    )

  def test_minimize_no_change_subroutine_type(self):
    ir_file = self.create_tempfile(content=ARRAY_IR)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(
        test_sh_file.full_path, ['/usr/bin/env grep invoke $1']
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params=false',
        ir_file.full_path,
    ])
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(minimized_ir.decode('utf-8'), """package foo

fn bar(x: bits[8][8]) -> bits[8][4] {
  ret literal.57: bits[8][4] = literal(value=[0, 0, 0, 0], id=57)
}

top fn foo(x: bits[8], y: bits[8]) -> bits[8][2] {
  literal.62: bits[8][8] = literal(value=[0, 0, 0, 0, 0, 0, 0, 0], id=62)
  invoke.26: bits[8][4] = invoke(literal.62, to_apply=bar, id=26)
  literal.64: bits[1] = literal(value=0, id=64)
  ret array_slice.65: bits[8][2] = array_slice(invoke.26, literal.64, width=2, id=65)
}
""")

  def test_minimize_add_no_remove_params(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, ['/usr/bin/env grep add $1'])
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params=false',
        ir_file.full_path,
    ])
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

top fn foo(x: bits[32], y: bits[32]) -> bits[32] {
  literal.15: bits[32] = literal(value=0, id=15)
  ret add.2: bits[32] = add(literal.15, literal.15, id=2)
}
""",
    )

  def test_minimize_add_remove_params(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, ['/usr/bin/env grep add $1'])
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params',
        ir_file.full_path,
    ])
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

top fn foo() -> bits[32] {
  literal.13: bits[32] = literal(value=0, id=13)
  ret add.2: bits[32] = add(literal.13, literal.13, id=2)
}
""",
    )

  def test_no_reduction_possible(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    test_sh_file = self.create_tempfile()
    # Shell script is run with -e so if any of the greps fail then the script
    # fails.
    self._write_sh_script(
        test_sh_file.full_path,
        [
            '/usr/bin/env grep not.1.*x $1',
            '/usr/bin/env grep add.2.*not.1.*y $1',
            '/usr/bin/env grep not.3.*add.2 $1',
        ],
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        '--can_remove_params',
        ir_file.full_path,
    ])
    self.assertEqual(minimized_ir.decode('utf-8'), ADD_IR)

  def test_simplify_and_unbox_array(self):
    input_ir = """package foo

top fn foo(x: bits[32], y: bits[32]) -> bits[32][3] {
  not: bits[32] = not(x, id=3)
  ret a: bits[32][3] = array(x, y, not)
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if a not(x) instruction is in the IR.
    self._write_sh_script(
        test_sh_file.full_path, ['/usr/bin/env grep not.*x $1']
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        ir_file.full_path,
    ])
    # The array operation should have been stripped from the function.
    self.assertIn('array(', input_ir)
    self.assertNotIn('array(', minimized_ir.decode('utf-8'))

  def test_simplify_tuple(self):
    input_ir = """package foo

top fn foo(x: bits[32], y: bits[32], z: bits[32]) -> (bits[32], (bits[32], bits[32]), bits[32]) {
  tmp: (bits[32], bits[32]) = tuple(y, x)
  ret a: (bits[32], (bits[32], bits[32]), bits[32]) = tuple(y, tmp, z)
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if a tuple(... x ...) instruction is in
    # the IR. A single element tuple containing x should remain.
    self._write_sh_script(
        test_sh_file.full_path, ['/usr/bin/env grep "tuple(.*x" $1']
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        ir_file.full_path,
    ])
    self.assertRegex(
        minimized_ir.decode('utf-8'),
        r'ret tuple\.\d+: \(bits\[32\]\) = tuple\(x, id=\d+\)',
    )

  def test_simplify_array(self):
    input_ir = """package foo

top fn foo() -> bits[32][3] {
  ret a: bits[32][3] = literal(value=[0, 0, 0], id=3)
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(
        test_sh_file.full_path, [r'/usr/bin/env grep "bits\[32\]\[[123]\]" $1']
    )
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertRegex(
        minimized_ir,
        r'ret \w+.\d+: bits\[32\]\[1\] = literal\(value=\[0\], id=\d+\)',
    )

  def test_proc(self):
    input_ir = '''package foo

chan input(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="""""")
chan output(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="""""")

top proc foo(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
'''
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertIn('proc foo', minimized_ir)

  def test_proc_remove_sends(self):
    input_ir = '''package foo

chan input(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="""""")
chan output(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="""""")

top proc foo(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
'''
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--can_remove_sends',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertIn('receive', minimized_ir)
    self.assertNotIn('send', minimized_ir)

  def test_remove_receives(self):
    input_ir = '''package foo

chan input(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="""""")
chan output(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="""""")

top proc foo(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
'''
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--can_remove_receives',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertNotIn('receive', minimized_ir)
    self.assertIn('send', minimized_ir)

  def test_proc_remove_sends_and_receives(self):
    input_ir = '''package foo

chan input(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="""""")
chan output(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="""""")

top proc foo(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
'''
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--can_remove_receives',
            '--can_remove_sends',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertNotIn('receive', minimized_ir)
    self.assertNotIn('send', minimized_ir)

  def test_proc_preserve_channels(self):
    input_ir = '''package foo

chan input(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="""""")
chan output(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="""""")

top proc foo(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
'''
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--can_remove_receives',
            '--can_remove_sends',
            '--preserve_channels=input',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertIn('chan input', minimized_ir)
    self.assertNotIn('chan output', minimized_ir)

  def test_new_style_proc(self):
    input_ir = """package foo

top proc foo<input: bits[32] streaming in, output:bits[32] streaming out>(foo: bits[32], bar: bits[32], baz: bits[32], init={1, 2, 3}) {
  tkn: token = literal(value=token, id=1000)
  receive.1: (token, bits[32]) = receive(tkn, channel=input)
  tuple_index.2: token = tuple_index(receive.1, index=0)
  tuple_index.3: bits[32] = tuple_index(receive.1, index=1)
  send.4: token = send(tkn, baz, channel=output)
  next (tuple_index.3, foo, bar)
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, [r'/usr/bin/env'])  # = true
    # The minimizer should not crash.
    subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--can_remove_receives',
            '--can_remove_sends',
            '--preserve_channels=input',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )

  def test_verify_return_code(self):
    # If the test script never successfully runs, then ir_minimizer_main should
    # return nonzero.
    ir_file = self.create_tempfile(content=ADD_IR)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, ['exit 1'])
    with self.assertRaises(subprocess.CalledProcessError):
      subprocess.check_call([
          IR_MINIMIZER_MAIN_PATH,
          '--test_executable=' + test_sh_file.full_path,
          '--can_remove_params',
          ir_file.full_path,
      ])

  def test_minimize_jit_mismatch(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_llvm_jit',
            '--use_optimization_pipeline',
            '--input=bits[32]:0x42; bits[32]:0x123',
            '--test_only_inject_jit_result=bits[32]:0x22',
            ir_file.full_path,
        ],
        stderr=subprocess.PIPE,
    )
    # The minimizer should reduce the test case to just a literal.
    self.assertIn('ret literal', minimized_ir.decode('utf-8'))

  def test_minimize_jit_mismatch_but_no_mismatch(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    comp = subprocess.run(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_llvm_jit',
            '--use_optimization_pipeline',
            '--input=bits[32]:0x42; bits[32]:0x123',
            ir_file.full_path,
        ],
        stderr=subprocess.PIPE,
        check=False,
    )
    self.assertNotEqual(comp.returncode, 0)
    self.assertIn(
        'main function provided does not fail', comp.stderr.decode('utf-8')
    )

  def test_remove_userless_sideeffecting_op(self):
    input_ir = """package foo

top fn foo(x: bits[32], y: bits[1]) -> bits[32] {
  gate_node: bits[32] = gate(y, x)
  ret test_node: bits[32] = identity(x)
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if `test_node` is in the IR.
    self._write_sh_script(
        test_sh_file.full_path, ['/usr/bin/env grep test_node $1']
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        ir_file.full_path,
    ])
    self.assertNotIn('gate_node', minimized_ir.decode('utf-8'))

  def test_remove_literal_subelements(self):
    input_ir = """package foo

top fn foo() -> (bits[1], (bits[42]), bits[32]) {
  ret result: (bits[1], (bits[42]), bits[32]) = literal(value=(0, (0), 0))
}
"""
    ir_file = self.create_tempfile(content=input_ir)
    test_sh_file = self.create_tempfile()
    # The test script checks to see if `bits[42]` is in the IR.
    self._write_sh_script(
        test_sh_file.full_path, ['/usr/bin/env grep bits.42 $1']
    )
    minimized_ir = subprocess.check_output([
        IR_MINIMIZER_MAIN_PATH,
        '--test_executable=' + test_sh_file.full_path,
        ir_file.full_path,
    ])
    # All tuple elements but bits[42] should be removed.
    self.assertIn(': ((bits[42])) = literal', minimized_ir.decode('utf-8'))

  def test_proc_works_with_multiple_simplifications_between_tests(self):
    ir_file = self.create_tempfile(content=ADD_IR)
    test_sh_file = self.create_tempfile()
    self._write_sh_script(test_sh_file.full_path, ['/usr/bin/env grep add $1'])
    # Minimizing with small simplifications_between_tests should work.
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--simplifications_between_tests=2',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertNotIn('not', minimized_ir)

    # Minimizing with large simplifications_between_tests won't work for this
    # small example (it will remove everything), so check that the IR is
    # unchanged.
    minimized_ir = subprocess.check_output(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            '--can_remove_params',
            '--simplifications_between_tests=100',
            ir_file.full_path,
        ],
        encoding='utf-8',
    )
    self.assertEqual(ADD_IR, minimized_ir)

  def test_inline_single_invoke_is_triggerable(self):
    ir_file = self.create_tempfile(content=INVOKE_TWO)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if `invoke.2` is in the IR.
    self._write_sh_script(
        test_sh_file.full_path,
        ["/usr/bin/env grep 'invoke(x, to_apply=bar, id=6)' $1"],
    )
    output = subprocess.run(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--test_executable=' + test_sh_file.full_path,
            ir_file.full_path,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    self.assertEqual(
        output.returncode,
        0,
        f'Non zero return: stderr {output.stderr}, stdout: {output.stdout}',
    )
    minimized_ir = output.stdout
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

fn bar(x: bits[32]) -> bits[1] {
  ret literal.12: bits[1] = literal(value=0, id=12)
}

top fn foo(x: bits[32], y: bits[32]) -> bits[1] {
  ret invoke.6: bits[1] = invoke(x, to_apply=bar, id=6)
}
""",
    )

  def test_can_remove_invoke_args(self):
    ir_file = self.create_tempfile(content=INVOKE_TWO)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if invoke of bar is in the IR.
    self._write_sh_script(
        test_sh_file.full_path, ["/usr/bin/env grep 'to_apply=baz' $1"]
    )
    output = subprocess.run(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--can_remove_params',
            '--can_inline_everything=false',
            '--test_executable=' + test_sh_file.full_path,
            ir_file.full_path,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    self.assertEqual(
        output.returncode,
        0,
        f'Non zero return: stderr {output.stderr}, stdout: {output.stdout}',
    )
    minimized_ir = output.stdout
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

fn baz() -> bits[1] {
  ret literal.28: bits[1] = literal(value=0, id=28)
}

top fn foo() -> bits[1] {
  ret invoke.29: bits[1] = invoke(to_apply=baz, id=29)
}
""",
    )

  def test_can_unwrap_map(self):
    ir_file = self.create_tempfile(content=INVOKE_MAP)
    test_sh_file = self.create_tempfile()
    # The test script only checks to see if bar is present
    self._write_sh_script(
        test_sh_file.full_path,
        ['/usr/bin/env grep bar $1'],
    )
    output = subprocess.run(
        [
            IR_MINIMIZER_MAIN_PATH,
            '--can_remove_params',
            '--can_inline_everything=false',
            '--test_executable=' + test_sh_file.full_path,
            ir_file.full_path,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    self.assertEqual(
        output.returncode,
        0,
        f'Non zero return: stderr {output.stderr}, stdout: {output.stdout}',
    )
    minimized_ir = output.stdout
    self._maybe_record_property('output', minimized_ir.decode('utf-8'))
    self.assertEqual(
        minimized_ir.decode('utf-8'),
        """package foo

fn bar() -> bits[1] {
  ret literal.20: bits[1] = literal(value=0, id=20)
}

top fn foo() -> bits[1] {
  ret invoke.21: bits[1] = invoke(to_apply=bar, id=21)
}
""",
    )


if __name__ == '__main__':
  absltest.main()
