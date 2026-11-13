# A block's last statement is the value of the block, which a method of
# the program's of an iterator's name may take (`def each = p(yield)`):
# the builtin each, times, loop, step, upto, downto and each_with_index
# throw it away, and so does a method of the program's that throws away
# each of its yields' values (Lines#each below).

class Pair
  def to_ary = [1, 2]
end

class Box
  def each = p(yield.class)
  def each_with_index = p(yield(:e, 0))
  def self.each = p(yield)
end

# a multiple assignment answers its right side, not the Array it spread
Box.new.each { a, b = Pair.new }
[1].each { a, b = Pair.new }
p((a, b = Pair.new).class)

class Integer
  def times = p(yield(0))
end
3.times { 7 }

def loop = p(yield)
loop { :lv }

class R
  def step(n) = p(yield(n))
  def upto(n) = p(yield(n))
end
R.new.step(2) { |i| i * 10 }
R.new.upto(3) { |i| i + 1 }
Box.new.each_with_index { |x, i| [x, i] }
Box.each { :sv }

class Lines
  def initialize(n) = @n = n
  def each
    i = 0
    while i < @n
      yield i
      i += 1
    end
  end
end
r = []
Lines.new(3).each { |i| r << i if i > 0 }
p r
Lines.new(1).each { a, b = Pair.new; p [a, b] }

# a builtin iterator still answers its receiver
r = [1, 2].each { |x| x * 2 }
p r
p((1..2).each { |x| x })
