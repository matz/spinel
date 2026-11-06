#!/bin/sh
# N constants, each written a Struct.new whose block defines one method and
# makes it private and public again 128 times, and sixteen top-level defs per
# constant. None of the calls can show the class, so the pass that marks a
# block's class anonymous until its write names it
# (desugar_struct_block_anonymous) asks of every call whether the program
# defines a method of its name anywhere. A scan of the program's defs per call,
# or a buffer of the node table's size per block, grows as N squared.
n=${1:-100}
awk -v n="$n" 'BEGIN {
  for (i = 0; i < n; i++) {
    printf "S%d = Struct.new(:a, :b) do\n", i
    printf "  def m%d = a\n", i
    for (j = 0; j < 128; j++) printf "  %s :m%d\n", (j % 2 ? "public" : "private"), i
    print "end"
    for (j = 0; j < 16; j++) printf "def f%d_%d(x) = x + %d\n", i, j, j
  }
  for (i = 0; i < n; i++) printf "p S%d.new(%d, 1).m%d + f%d_0(%d)\n", i, i, i, i, i
}'
