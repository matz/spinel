# A yield-set ivar is boxed, while its combinatorial block can have a typed row.
class YieldRows
  attr_reader :got
  def each = (@got = yield)
end
bx = YieldRows.new
bx.each { [3.5, 1.5] }
bx.got.permutation(2) { |q| p q }
bx.got.permutation { |q| p q }
bx.got.combination(1) { |q| p q }
bx.got.repeated_permutation(2) { |q| p q }
bx.got.repeated_combination(2) { |q| p q }
bx.got.permutation(0) { |q| p q }
class IntegerRows
  attr_reader :got
  def each = (@got = yield)
end
ix = IntegerRows.new
ix.each { [3, 1] }
ix.got.permutation(2) { |q| p q }
ix.got.combination(1) { |q| p q }
