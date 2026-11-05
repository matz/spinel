#!/bin/sh
# N classes that each extend one module, read its method in the body, then
# define the method on their own singleton: each own def shadows the
# extend's copy of a version a call has met, so the scope builder asks the
# load record once per class whether a call meets the module's version
# (elab_version_met). A question that reads every call of the name grows as
# N squared. With a second argument, `plain`, the classes extend nothing
# and read no method: the program around the questions, whose work the
# scale test subtracts.
N=${1:-10}
awk -v N="$N" -v P="${2:-}" 'BEGIN {
  print "module M"
  print "  def v = 1"
  print "end"
  for (i = 0; i < N; i++) {
    printf "class C%d\n", i
    if (P == "") print "  extend M"
    if (P == "") print "  X = v"
    else print "  X = 1"
    printf "  def self.v = %d\n", i
    print "end"
  }
  print "s = 0"
  for (i = 0; i < N; i++) printf "s += C%d.v + C%d::X\n", i, i
  print "p s"
}'
