#!/bin/sh
# N readonly POLY entry parameters share an N-method suffix. Both Ruby and
# emitted C grow linearly; promotion must not walk that suffix for every root.
awk -v N="${1:-50}" 'BEGIN {
  for (i = 0; i < N; i++)
    printf "def entry%d(value) = suffix0(value)\n", i
  for (i = 0; i < N - 1; i++)
    printf "def suffix%d(value); suffix%d(value); 0; end\n", i, i + 1
  printf "def suffix%d(value); value.bytesize if value.is_a?(String); 0; end\n", N - 1
  print "text = +\"abc\""
  for (i = 0; i < N; i++) {
    # Seed every vertex directly: measure forwarding, not N rounds of return
    # type propagation or a warning from the unrelated inference round bound.
    printf "suffix%d(1); suffix%d(text)\n", i, i
    printf "entry%d(1)\n", i
    printf "raise \"wrong readonly result\" unless entry%d(text) == 0\n", i
  }
  print "raise \"modified caller\" unless text == \"abc\""
}'
