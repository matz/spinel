# A walk the program redefines may read what its block answers: a tuple
# taken apart as such a block's last statement stays boxed and answers as
# CRuby does.
# spinel: gc-stress

class Integer
  def times
    i = 0
    while i < self
      $times_got = yield(i)
      i += 1
    end
    self
  end
end

module Enumerable
  def each_with_index
    i = 0
    each { |x| $ewi_got = yield(x, i); i += 1 }
    self
  end
end

def times_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
tm_a = nil
1.times { |i| tm_a, tm_b = times_pair }
tm_a[0][0] += 1
$times_got[0][1][1] = "t"
p tm_a, $times_got

def ewi_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 3]
end
ew_a = nil
[1].each_with_index { |x, i| ew_a, ew_b = ewi_pair }
ew_a[0][0] += 2
$ewi_got[0][1][0] = "e"
p ew_a, $ewi_got
