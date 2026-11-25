#!/bin/sh
# Class-body macros: N classes, each calling a superclass's macro ten
# times, compiled where the registration's walk
# runs each call and splices the define_method it makes. A walk-time
# splice that rebuilds anything of the whole program (a parent map, the
# kind lists, the load record) grows as N^2.
N=${1:-10}
awk -v N="$N" 'BEGIN {
  print  "class Base"
  print  "  def self.prop(n)"
  print  "    define_method(\"get_#{n}\") { n.to_s }"
  print  "  end"
  print  "end"
  for (i = 0; i < N; i++) {
    printf "class C%d < Base\n", i
    for (k = 0; k < 10; k++) printf "  prop :p%d\n", k
    print  "end"
  }
  print  "s = 0"
  for (i = 0; i < N; i++) printf "s += C%d.new.get_p%d.size\n", i, i % 10
  print  "p s"
}'
