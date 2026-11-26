# A boxed receiver whose class answers transform_values through
# method_missing calls it there: the Hash rewrite of
# transform_values.with_index is left out in such a program.
class W
  def initialize(tag) = @tag = tag
  def with_index = [@tag]
end
class MM
  def method_missing(n, *a, &b) = (b ? :with_block : W.new(n))
  def respond_to_missing?(*) = true
end

def order(k) = k.transform_values.with_index { |v, i| [v, i] }

p order(MM.new)
p order([MM.new, nil][1]) rescue p $!.class
