#!/bin/sh
# N call sites that desugar_missing_method_calls asks of String's chain or
# splits, with String reopened N/2 times, each time including a module:
# a third call a name a String mixin defines and undefines on a String or
# on a class that defines it, a third call a name a class's method_missing
# answers on a local holding that class or one that defines it, and a third
# call a name String undefines and its own method_missing answers. Each
# site's ask of String's chain walked the chain again, each split
# relowered over the whole table, and each split's String arm rebuilt the
# load record's map, so the pass grew as N^2 (#12542).
N=${1:-10}
awk -v N="$N" 'BEGIN {
  k = int(N / 10); if (k < 1) k = 1
  m = int(N / 2); if (m < 1) m = 1
  print "module Undefs; def size = 0; undef_method :size; end"
  print "class String; include Undefs; end"
  for (i = 0; i < m; i++) {
    printf "module Ext%d; def ext%d = %d; end\n", i, i, i
    printf "class String; include Ext%d; def str_helper%d = %d; end\n", i, i, i
  }
  print "class String"
  print "  def method_missing(name, *args) = name == :upcase ? 7 : super"
  print "  def respond_to_missing?(name, priv = false) = name == :upcase || super"
  print "  undef_method :upcase"
  print "end"
  for (j = 0; j < k; j++) {
    printf "class C%d\n  def size = %d\n  def upcase = %d\n", j, j, j
    printf "  def method_missing(name, *args) = name == :zz ? %d : super\n", j
    printf "  def respond_to_missing?(name, priv = false) = name == :zz || super\nend\n"
    printf "class D%d; def zz = -%d; end\n", j, j
  }
  for (i = 0; i < N; i++) {
    if (i % 3 == 0) printf "def site%d(x) = (x.even? ? \"s%d\" : C%d.new).size\n", i, i, i % k
    else if (i % 3 == 1) printf "def site%d(x) = (y = x.even? ? C%d.new : D%d.new; y.zz)\n", i, i % k, i % k
    else printf "def site%d(x) = (x.even? ? \"s%d\" : C%d.new).upcase\n", i, i, i % k
  }
  print "t = 0"
  for (i = 0; i < N; i++) printf "t += site%d(%d)\n", i, i
  print "p t"
}'
