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

proc parametric<N: u32, M: u32> {
    c: chan<uN[M]> in;
    s: chan<uN[M]> out;

    config(c: chan<uN[M]> in, s: chan<uN[M]> out) { (c, s) }

    init { () }

    next(state: ()) {
        let (tok, input) = recv(join(), c);
        let output = ((input as uN[N]) * uN[N]:2) as uN[M];
        let tok = send(tok, s, output);
    }
}

#[test_proc]
proc test_proc {
    terminator: chan<bool> out;
    output_c: chan<u37> in;
    input_p: chan<u37> out;

    config(terminator: chan<bool> out) {
        let (input_p, input_c) = chan<u37>("input");
        let (output_p, output_c) = chan<u37>("output");
        spawn parametric<u32:32, u32:37>(input_c, output_p);
        (terminator, output_c, input_p)
    }

    init { () }

    next(state: ()) {
        let tok = send(join(), input_p, u37:1);
        let (tok, result) = recv(tok, output_c);
        assert_eq(result, u37:2);

        let tok = send(tok, input_p, u37:8);
        let (tok, result) = recv(tok, output_c);
        assert_eq(result, u37:16);

        let tok = send(tok, terminator, true);
    }
}
