# flat_map splices a block result that answers to_ary, as CRuby's
# rb_check_array_type does (an ActiveRecord relation is one); a to_ary that
# answers nil, and a value without one, are pushed as themselves.
class Bag
  def initialize(items)
    @items = items
  end

  def to_ary
    @items
  end
end

class Opaque
  def to_ary
    nil
  end

  def inspect
    "#<Opaque>"
  end
end

p [1, 2].flat_map { |n| Bag.new([n, n * 10]) }
p [1, 2].collect_concat { |n| Bag.new([n]) }
p [1, 2, 3].flat_map { |n| n.odd? ? [n, n] : n }
p %w[a b].flat_map { |s| s * 2 }
p [1].flat_map { |_| Opaque.new }

# lazy flat_map splices the same way, and a to_ary that gives a non-Array
# (not nil) is a TypeError, eager or lazy
class LPair; def to_ary = [:a, :b]; end
class LNoConv; def to_ary = nil; def inspect = "LNoConv"; end
class BadConv; def to_ary = 5; def inspect = "BadConv"; end
class StrConv; def to_ary = "s"; end

p [1, 2].flat_map { LPair.new }
p [1, 2].lazy.flat_map { LPair.new }.to_a
p [1, 2].lazy.collect_concat { LPair.new }.first(3)
p (1..3).lazy.flat_map { |i| i.even? ? LPair.new : i }.to_a
p [1].flat_map { LNoConv.new }
p [1].lazy.flat_map { LNoConv.new }.to_a

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

class SubA < Array; end
class GivesSub; def to_ary = SubA[7, 8]; end
p [1].flat_map { GivesSub.new }
p [1].lazy.flat_map { GivesSub.new }.to_a

class Gone; def to_ary = [:conv]; end
class Gone; undef_method :to_ary; end
p [1].lazy.flat_map { Gone.new }.to_a.map(&:class)
p [1].flat_map { Gone.new }.map(&:class)
