#!/bin/sh
# Class-body macros that keep state: one class of K calls that set it
# (`kind :kI`) and K that read it to build a constant (`constant :CI`),
# compiled under --load-model=graph. Each reading call follows the class's
# state over the statements before it (ct_class_state): from where the
# last call stood, so the body is followed once; from the body's start at
# each call, it grows as K^2.
K=${1:-10}
awk -v K="$K" 'BEGIN {
  print  "module Consts"
  print  "  def kind(k = nil)"
  print  "    return @kind if k.nil?"
  print  "    @kind = k"
  print  "  end"
  print  "  def constant(c) = const_set(c, \"#{kind}_#{c}\")"
  print  "end"
  print  "class Calc"
  print  "  extend Consts"
  for (i = 0; i < K; i++) { printf "  kind :k%d\n", i; printf "  constant :C%d\n", i }
  print  "end"
  printf "p Calc::C0, Calc::C%d\n", K - 1
}'
