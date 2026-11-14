#!/bin/sh
# N classes that each write a constant and read it, from a method of the
# class, from a class nested in it and from the top level. With "N colliding"
# every class's constant is SIZE, so each collides with the others; with "N
# distinct" class i's is SIZE_i, the same program with no collision. Every
# class's nested In collides with the others in both. A builder B<i> =
# Class.new beside a header class B<i> is checked against the headers and
# writes of its name (qc_refuse_conditional_builder). The load record keys
# each write and each read after the walk (elab_const_keys) from the entity it
# names, so the work grows with N; a read keyed by scanning every write of its
# name grows as N^2. make scale-test takes the work of those steps alone
# (spinel-work-step) in the colliding form.
[ $# -eq 1 ] && set -- $1   # "N mode" as one argument too
N=${1:-10}; MODE=${2:-colliding}
awk -v N="$N" -v MODE="$MODE" 'BEGIN {
  for (i = 0; i < N; i++) {
    c = MODE == "distinct" ? sprintf("SIZE_%d", i) : "SIZE"
    printf "class C%d\n", i
    printf "  %s = %d\n", c, i
    printf "  def self.size = %s\n", c
    printf "  class In\n    def self.size = %s\n  end\n", c
    printf "end\n"
    printf "B%d = Class.new\nclass B%d\n  def self.b = %d\nend\n", i, i, i
  }
  print "t = 0"
  for (i = 0; i < N; i++) {
    c = MODE == "distinct" ? sprintf("SIZE_%d", i) : "SIZE"
    printf "t += C%d.size + C%d::In.size + C%d::%s + B%d.b\n", i, i, i, c, i
  }
  print "p t"
}'
