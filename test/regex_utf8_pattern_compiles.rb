# Issue #61 stage 1: a regex source containing multi-byte UTF-8 chars
# must reach the runtime with the correct byte length. With character-
# count length, the engine truncated mid-multibyte at startup and
# raised "unterminated character class" before any matching could run.
#
# This test only checks that compiling the binary and running it past
# the regex initialization is clean — full UTF-8 matching support is a
# separate stage.

re1 = /[₀₁₂₃₄₅₆₇₈₉]+/
re2 = /\A→\z/
re3 = /[αβγ]/

puts "ok"
