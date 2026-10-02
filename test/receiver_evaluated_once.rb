# A call on a value an iterator computes evaluates that iterator once. The
# String, Integer and Float receivers' emitter emitted the receiver's work
# before it knew whether it answers the method, and a method the program
# defines on Object or Numeric is answered further on, where the receiver was
# emitted again: the min block ran twice over, and the ruby/spec harness's
# `x.should == v` ran every side-effecting x twice.
class Object
  def tw_o = [self, self]
end
class Numeric
  def tw_n = [self, self]
end
module Comparable
  def tw_c = [self, self]
end
@i = 0
p [3, 1, 2].min { |a, b| @i += 1; a <=> b }.tw_o, @i
@i = 0
p [3, 1, 2].map { |x| @i += 1; x.to_s }.first.tw_o, @i
@i = 0
p [1.5, 2.5].max { |a, b| @i += 1; a <=> b }.tw_o, @i
@i = 0
p [3, 1, 2].min { |a, b| @i += 1; a <=> b }.tw_n, @i
@i = 0
p [3, 1, 2].min { |a, b| @i += 1; a <=> b }.tw_c, @i
@i = 0
p [3, 1, 2].count { |x| @i += 1; x > 1 }.tw_o, @i
$c = 0
def bump = ($c += 1)
p bump.tw_o, $c
p (bump + 1).tw_o, $c
