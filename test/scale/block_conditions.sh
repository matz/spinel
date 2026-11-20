#!/bin/sh
# N classes, each with a method that tests its own block parameter as the
# condition of an if (`@v += 1 if blk`, as the DSL methods gems define do
# before an `instance_eval(&blk)`), then a call of each with a block: the
# class structure phase turns such a condition into block_given? where the
# method never writes blk and reads it for nothing but such a condition or
# a forward to instance_eval. Those checks read the method's own local
# writes and reads; a walk of the whole node table for them per method
# grows as N^2. With "N plain" the condition is the method's other
# parameter, which the pass does not consider: the difference is the pass's
# own work. (An instance variable as that condition was one more read of
# @v than the block's program has, which the analysis of instance
# variables counts: it outweighed the pass's work. No
# forward here: the block_given? the pass makes changes what the later
# passes do with a forwarded block, which the difference would then count.)
[ $# -eq 1 ] && set -- $1   # "N mode" as one argument too
N=${1:-10}; MODE=${2:-blk}
awk -v N="$N" -v MODE="$MODE" 'BEGIN {
  cond = MODE == "plain" ? "q" : "blk"
  for (i = 0; i < N; i++)
    printf "class C%d\n  def initialize = @v = %d\n  def run(q = 1, &blk)\n    @v += 1 if %s\n    @v\n  end\nend\n", i, i, cond
  print "t = 0"
  for (i = 0; i < N; i++) printf "t += C%d.new.run { 1 }\n", i
  print "p t"
}'
