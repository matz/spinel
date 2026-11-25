#!/bin/sh
# Class-body macros in one class: K calls to a superclass's macro that
# defines two methods each, and (with a second argument, `written`) the
# same 2K define_methods written out in the class. The walk splices each
# call's methods into the one body; a row that walks the whole body per
# splice, rather than once per body, makes the macro
# program cost K^2 more than the written one. (The pair compares the two
# at one K: K methods in one class already cost more than linearly in the
# analysis, macros or not.)
K=${1:-10}
awk -v K="$K" -v W="${2:-}" 'BEGIN {
  print  "class Base"
  print  "  def self.prop(n, v)"
  print  "    k = \"#{n}_k\""
  print  "    define_method(\"get_#{n}\") { |x = 0| v + x }"
  print  "    define_method(\"#{k}?\") { v > 3 }"
  print  "  end"
  print  "end"
  print  "class C < Base"
  for (i = 1; i <= K; i++) {
    if (W == "") printf "  prop :p%d, %d\n", i, i
    else {
      printf "  define_method(:get_p%d) { |x = 0| %d + x }\n", i, i
      printf "  define_method(:p%d_k?) { %d > 3 }\n", i, i
    }
  }
  print  "end"
  print  "p C.new.get_p1(1), C.new.p1_k?"
}'
