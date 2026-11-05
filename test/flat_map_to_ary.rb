# flat_map splices a block result that answers #to_ary as that Array, eager
# or lazy. A to_ary that gives nil leaves the value as one element, and one
# that gives any other non-Array is a TypeError.

class Pair; def to_ary = [:a, :b]; end
class NoConv; def to_ary = nil; def inspect = "NoConv"; end
class BadConv; def to_ary = 5; def inspect = "BadConv"; end
class StrConv; def to_ary = "s"; end

p [1, 2].flat_map { Pair.new }
p [1, 2].lazy.flat_map { Pair.new }.to_a
p [1, 2].lazy.collect_concat { Pair.new }.first(3)
p (1..3).lazy.flat_map { |i| i.even? ? Pair.new : i }.to_a
p [1].flat_map { NoConv.new }
p [1].lazy.flat_map { NoConv.new }.to_a

begin
  [1].flat_map { BadConv.new }
rescue TypeError => e
  puts e.message
end
begin
  [1].lazy.flat_map { BadConv.new }.to_a
rescue TypeError => e
  puts e.message
end
begin
  (1..2).flat_map { StrConv.new }
rescue TypeError => e
  puts e.message
end
begin
  { k: 1 }.flat_map { BadConv.new }
rescue TypeError => e
  puts e.message
end

p [[1], 2].flat_map { |x| x }
p [1, 2].lazy.flat_map { |i| { i => i } }.to_a
