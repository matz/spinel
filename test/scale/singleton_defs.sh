#!/bin/sh
# N singleton defs on one class, and sixteen calls per def of a method a class
# object answers with an arity of its own (Module#name takes none), each given
# an argument: the arity check of every call asks whether the program defines
# that method on the constant's singleton, sixteen times per call (fifteen
# inference passes while its type is unknown, and the emission). A list of the defs that is scanned for
# each ask grows as N squared. The calls raise ArgumentError when run; the test
# compiles them and runs nothing.
n=${1:-100}
awk -v n="$n" 'BEGIN {
  print "class SingletonScale; end"
  for (i = 0; i < n; i++) printf "def SingletonScale.m_%d; %d; end\n", i, i
  for (i = 0; i < n * 16; i++) print "result = SingletonScale.name(1)"
}'
