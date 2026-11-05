#!/bin/sh
# Module functions: N modules of ten functions written after a bare
# `module_function`, N modules of ten copied by name (`module_function :g0,
# ...`), each function calling the next one of its module and, every third,
# the last function of the next module; a class per pair that extends both and
# later defines one of the names on its own singleton; and a top-level loop
# that calls the modules' functions and the classes'. Each copy is a version of
# the singleton's method that the load record holds and the call plan maps to
# the def's scope, so a pass that rescans the record, the scopes or the copies
# per copy or per call grows as N^2. With a second argument, `read`, each
# class reads both names in its body before its own defs: the extended
# version is the one a call meets there, so the extend's copy is kept behind
# the own def and the call is bound to it, and a pass that asks every call
# of a name per kept copy grows as N^2 too.
N=${1:-10}
awk -v N="$N" -v R="${2:-}" 'BEGIN {
  for (i = 0; i < N; i++) {
    printf "module M%d\n", i
    print  "  module_function"
    for (k = 0; k < 10; k++) {
      printf "  def f%d\n", k
      if (k < 9) printf "    r = f%d + 1\n", k + 1
      else printf "    r = %d\n", i
      if (k % 3 == 0 && i + 1 < N) printf "    r += M%d.f9\n", i + 1
      print  "    r"
      print  "  end"
    }
    print  "end"
  }
  for (i = 0; i < N; i++) {
    printf "module W%d\n", i
    for (k = 0; k < 10; k++) {
      printf "  def g%d\n", k
      if (k < 9) printf "    r = g%d + 1\n", k + 1
      else printf "    r = %d\n", i
      if (k % 3 == 0 && i + 1 < N) printf "    r += W%d.g9\n", i + 1
      print  "    r"
      print  "  end"
    }
    printf "  module_function :g0, :g1, :g2, :g3, :g4, :g5, :g6, :g7, :g8, :g9\n"
    print  "end"
  }
  for (i = 0; i < N; i++) {
    printf "class C%d\n", i
    printf "  extend M%d\n", i
    printf "  extend W%d\n", i
    if (R != "") print  "  R = f3 + g4"
    print  "  def self.f3 = -1"
    print  "  def self.g4 = -1"
    print  "end"
  }
  print  "s = 0"
  print  "[1, 2].each do |t|"
  for (i = 0; i < N; i++) {
    printf "  s += M%d.f0 + M%d.f5 + W%d.g0 + W%d.g6 + C%d.f3 + C%d.g4 + t\n", i, i, i, i, i, i
  }
  print  "end"
  print  "p s"
}'
