#!/bin/sh
# N classes, each given a module by `K.include(M)` at the top level, a
# singleton module by `singleton_class.prepend(S)` in its body, and a module
# that extends itself (`extend self`), then a call of each: the load record
# takes each mixin at its point (mixin_by_record, elab_all_bodies), so the
# work grows with N; a mixin matched against every class's bodies grows as
# N^2. With "N plain" the same program has no mixin: the modules' methods
# are the classes' own.
[ $# -eq 1 ] && set -- $1   # "N mode" as one argument too
N=${1:-10}; MODE=${2:-mixins}
awk -v N="$N" -v MODE="$MODE" 'BEGIN {
  mx = MODE != "plain"
  for (i = 0; i < N; i++) {
    printf "module M%d; def m%d = %d; end\n", i, i, i
    printf "module S%d; def s%d = %d + super; end\n", i, i, i
    printf "module E%d; %sdef %se%d = %d; end\n", i, mx ? "extend self; " : "", mx ? "" : "self.", i, i
    printf "class K%d\n  def self.s%d = 1\n", i, i
    if (mx) printf "  singleton_class.prepend(S%d)\nend\nK%d.include(M%d)\n", i, i, i
    else printf "  def m%d = %d\nend\n", i, i
  }
  print "t = 0"
  for (i = 0; i < N; i++) printf "t += K%d.new.m%d + K%d.s%d + E%d.e%d\n", i, i, i, i, i, i
  print "p t"
}'
