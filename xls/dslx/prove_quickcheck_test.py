# Copyright 2024 The XLS Authors
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

"""Tests the prove_quickcheck_main command line utility."""

import subprocess as subp
from typing import Optional, Tuple

from xls.common import runfiles
from xls.common import test_base


_BINARY = runfiles.get_path('xls/dslx/prove_quickcheck_main')


class ProveQuickcheckMainTest(test_base.TestCase):

  def _prove_quickcheck(
      self,
      program: str,
      *,
      test_filter: Optional[str] = None,
      want_error: bool = False,
      alsologtostderr: bool = False,
      extra_flags: Tuple[str, ...] = (),
  ) -> Tuple[str, str]:
    temp_file = self.create_tempfile(content=program)
    cmd = [_BINARY, temp_file.full_path] + list(extra_flags)
    if test_filter is not None:
      cmd.append(f'--test_filter={test_filter}')
    if alsologtostderr:
      cmd.append('--alsologtostderr')
    p = subp.run(
        cmd,
        check=False,
        stdout=subp.PIPE,
        stderr=subp.PIPE,
        encoding='utf-8',
        env={},
    )
    if want_error:
      self.assertNotEqual(p.returncode, 0)
    else:
      self.assertEqual(p.returncode, 0, msg=p.stderr)
    return p.stdout, p.stderr

  def test_trivially_true(self):
    program = """
    #[quickcheck]
    fn qc_always_true() -> bool { true }
    """
    self._prove_quickcheck(program)

  def test_trivially_false(self):
    program = """
    #[quickcheck]
    fn qc_always_false() -> bool { false }
    """
    self._prove_quickcheck(program, want_error=True)

  def test_arithmetic_property(self):
    program = """
    #[quickcheck]
    fn qc_add_one(x: u4) -> bool { (x as u5 + u5:1) > (x as u5) }
    """
    self._prove_quickcheck(program)

  def test_never_42(self):
    program = """
    #[quickcheck]
    fn qc_never_42(x: u8) -> bool { x != u8:42 }
    """
    _, stderr = self._prove_quickcheck(program, want_error=True)
    self.assertIn('counterexample: bits[8]:42', stderr)

  def test_multiple_all_true(self):
    program = """
    #[quickcheck]
    fn qc_always_true() -> bool { true }
    #[quickcheck]
    fn qc_add_one(x: u4) -> bool { (x as u5 + u5:1) > (x as u5) }
    """
    self._prove_quickcheck(program)

  def test_multiple_one_false(self):
    program = """
    #[quickcheck]
    fn qc_always_true() -> bool { true }
    #[quickcheck]
    fn qc_add_one(x: u4) -> bool { (x as u5 + u5:1) > (x as u5) }
    #[quickcheck]
    fn qc_never_42(x: u8) -> bool { x != u8:42 }
    """
    _, stderr = self._prove_quickcheck(program, want_error=True)
    self.assertIn('counterexample: bits[8]:42', stderr)

  def test_multiple_one_false_filtered_out(self):
    program = """
    #[quickcheck]
    fn qc_always_true() -> bool { true }
    #[quickcheck]
    fn qc_add_one(x: u4) -> bool { (x as u5 + u5:1) > (x as u5) }
    #[quickcheck]
    fn qc_never_42(x: u8) -> bool { x != u8:42 }
    """
    self._prove_quickcheck(program, test_filter='qc_a*')


if __name__ == '__main__':
  test_base.main()
