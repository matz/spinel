# spinel: reject-super-new-block
# B's program `new` hands A#initialize a block through its super; `k.new` over `[B, A]`
# enters A#initialize without one. The block A#initialize hands to
# Array#min may be nil or not, so the call is refused rather than taken as
# always given one.
def least(a, &b) = a.min(&b)
class A
  attr_reader :r
  def initialize(a, &b) = @r = least(a, &b)
end
xs = ARGV.size > 5 ? "x" : [3, 1, 2]
class B < A; def self.new(a) = super(a) { |x, y| y <=> x }; end
[B, A].each { |k| p k.new(xs).r }
