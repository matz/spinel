# run-scale: 1000 | 16000 | 1.5
# A method whose block sits in the arm of a poly receiver's #each that a
# Hash never takes: a call over a Hash makes no closure, so it makes none of
# the block's cells either, and the bytes stay flat when such calls run 16x.
class Bag
  def each
    yield "z", 1
  end
end
def total(pairs, base)
  sum = base
  pairs.each { |name, value| sum += value + base }
  sum
end
p total(Bag.new, 1)
h = {}
n = 0
ARGV[0].to_i.times { |i| n += total(h, i) }
p n
