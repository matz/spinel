#!/bin/sh
# K layers of two class methods in one `class << self`, each calling both
# methods of the next layer bare, the last layer calling the first: a cycle
# of bare calls that branches at every method. Whether a class method takes
# its receiving class (cmethod_takes_self_cls) is decided once per strongly
# connected group of such calls; a walk that worked a method out again on
# each path into it grew as 2^K. Compiled to C at K=20 and K=40: the
# counted work grows linearly in K.
K=${1:-20}
awk -v K="$K" 'BEGIN {
  print "class R"
  print "  class << self"
  for (i = 0; i < K; i++) {
    nx = i + 1 < K ? i + 1 : 0
    split("x y", w, " ")
    for (j = 1; j <= 2; j++) {
      printf "    def %s%d(n)\n", w[j], i
      print "      return 0 if n <= 0"
      printf "      x%d(n - 1) + y%d(n - 1)\n", nx, nx
      print "    end"
    }
  }
  print "  end"
  print "end"
  print "class S < R; end"
  print "p S.x0(3)"
}'
