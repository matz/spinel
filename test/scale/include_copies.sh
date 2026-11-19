#!/bin/sh
# N classes, each including a module every class shares and a module of its
# own, then a call of each: register_includes copies each module's methods
# into the includer, and each copy grows the node table, after which
# intern_scope_ivars asks for the rebound marks (ie_rebound_marks). A walk
# of every call of the program for those marks, once per copied method,
# grows as N^2; only the instance_eval and instance_exec calls can carry
# such a block (ie_rb_scan). With "N plain" the same program has no
# include: the modules' methods are the classes' own.
[ $# -eq 1 ] && set -- $1   # "N mode" as one argument too
N=${1:-10}; MODE=${2:-includes}
awk -v N="$N" -v MODE="$MODE" 'BEGIN {
  mx = MODE != "plain"
  if (mx) {
    print "module Shared"
    print "  def s1 = 1"
    print "  def s2 = s1 + 1"
    print "end"
  }
  for (i = 0; i < N; i++) {
    if (mx) printf "module H%d\n  def h = %d\nend\nclass C%d\n  include Shared\n  include H%d\n", i, i, i, i
    else printf "class C%d\n  def s1 = 1\n  def s2 = s1 + 1\n  def h = %d\n", i, i
    print "  def b = h + s2"
    print "end"
  }
  print "t = 0"
  for (i = 0; i < N; i++) printf "t += C%d.new.b\n", i
  print "p t"
}'
