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
