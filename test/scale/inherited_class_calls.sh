#!/bin/sh
# N classes under a chain of six, each calling four class methods it
# inherits from the chain's root in its body, methods that keep no
# class-level state, then a call of each class's own methods: the class
# structure phase looks each call's name up the class's chain
# (specialize_inherited_cls_new, comp_name_answered) before the scope index
# is frozen, where a lookup scans every scope. A scan of the scopes for each
# call and each ancestor grows as N^2; one walk of the scopes per name
# answers every class (comp_scope_batch_begin). The scans read no node, so
# make scale-test takes this leg by the phase's CPU time.
N=${1:-10}
awk -v N="$N" 'BEGIN {
  print "class Base0"
  print "  def self.field(n) = n.to_s"
  print "  def self.index(n) = n.to_s.size"
  print "  def self.label(s) = s.upcase"
  print "end"
  for (d = 1; d <= 5; d++) printf "class Base%d < Base%d\n  def self.level%d = %d\nend\n", d, d - 1, d, d
  for (i = 0; i < N; i++) {
    printf "class C%d < Base5\n  field :a%d\n  field :b%d\n  index :c%d\n  label \"w%d\"\n", i, i, i, i, i
    printf "  def get%d = %d\n  def self.make%d = %d\nend\n", i, i, i, i
  }
  print "t = 0"
  for (i = 0; i < N; i++) printf "t += C%d.make%d + C%d.new.get%d\n", i, i, i, i
  print "p t"
}'
