#!/bin/sh
# Definers that have no load point: N classes whose class methods hold a `def`,
# a `def self.`, an `attr_reader`, a `define_method` and an `alias_method`; N
# lambdas that each hold a class body with a def; an at_exit with a class body;
# one class W whose N class methods define the same `w`, called in turn; and
# calls before and after at the top level and in N method bodies called from
# both. The load record keeps an extent for every such body (LrExt, built once
# after lr_run_points) and the call plan asks the run predicates over them, so a
# pass that rescans the bodies, the histories of a name or the program per body
# grows as N^2.
N=${1:-10}
awk -v N="$N" 'BEGIN {
  for (i = 0; i < N; i++) {
    printf "class U%d\n", i
    printf "  def self.add_%d = (def m = %d)\n", i, i
    printf "  def self.cadd_%d = (def self.c = %d)\n", i, i
    printf "  def self.rd_%d = (attr_reader :r%d)\n", i, i
    printf "  def self.dm_%d = (define_method(:d%d) { %d })\n", i, i, i
    printf "  def self.al_%d = (alias_method :a%d, :to_s)\n", i, i
    print  "end"
  }
  print "class W; end"
  for (i = 0; i < N; i++) printf "class W; def self.set_%d = (def w = %d); end\n", i, i
  for (i = 0; i < N; i++) printf "L%d = -> { class LC%d; def lm = %d; end }\n", i, i, i
  print "at_exit { class AE; def e = 1; end }"
  for (i = 0; i < N; i++) printf "def call_%d = (U%d.add_%d; W.set_%d)\n", i, i, i, i
  for (i = 0; i < N; i++) printf "U%d.add_%d\n", i, i
  for (i = 0; i < N; i++) printf "call_%d\n", i
  for (i = 0; i < N; i++) printf "L%d.call\n", i
  print "r = W.new.w"
}'
