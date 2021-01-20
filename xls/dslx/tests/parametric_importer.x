// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import xls.dslx.tests.parametric_import

type LocalType = parametric_import::Type<u32:1, u32:2>;

#![test]
fn parametric_importer() {
  // TODO(rspringer): This line fails to evaluate; the next one passes.
  // let foo: LocalType = parametric_import::Zero<u32:1, u32:2>();
  let foo = parametric_import::Zero<u32:1, u32:2>();
  ()
}
