# spinel: reject-super-new-block
# A Method of Class#new bound to B (`bind_call`) runs B's program `new`,
# whose super hands A#initialize a block, beside a visible `A.new` with one;
# the fact cannot see which `new` the Method runs, so it counts as entering
# A#initialize without one. The block A#initialize hands to Array#min may
# be nil or not, so the call is refused rather than taken as always given
# one.
def least(a, &b) = a.min(&b)
class A
  attr_reader :r
  def initialize(a, &b) = @r = least(a, &b)
end
xs = ARGV.size > 5 ? "x" : [3, 1, 2]
p A.new(xs) { |x, y| y <=> x }.r
class B < A; def self.new(a) = super(a) { |x, y| y <=> x }; end
p Class.instance_method(:new).bind_call(B, xs).r
