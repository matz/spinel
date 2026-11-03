#!/bin/sh
# N classes of a leaf name of their own, each in a module of its own (or, as
# "top", at the top level beside the empty module), and ten calls per class.
# qualify_colliding_classes decides for every nested class whether its leaf is
# shared, a native class's, or a builtin's; a pass that asks that by walking
# every call, or every other class, per class grows as N squared. The nested
# program's work less the top-level program's is what the nested classes cost
# (the calls and the classes are the same in both).
n=${1:-100}
where=${2:-nested}
awk -v n="$n" -v where="$where" 'BEGIN {
  for (i = 0; i < n; i++) {
    if (where == "top") printf "module Mod%d\nend\nclass Leaf%d\nend\n", i, i
    else printf "module Mod%d\n  class Leaf%d\n  end\nend\n", i, i
  }
  for (i = 0; i < n * 10; i++) print "nil.to_a"
}'
