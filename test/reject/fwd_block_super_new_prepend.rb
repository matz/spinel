# spinel: reject-super-new-block
# A program `new` whose super reaches the builtin Class#new enters
# A#initialize too, here without a block (a module's `new` prepended to B's singleton class), beside a visible
# A.new with one: the block A#initialize hands to Array#min may be nil
# or not, so the call is refused rather than taken as always given one.
def least(a, &b) = a.min(&b)
class A
  attr_reader :r
  def initialize(a, &b) = @r = least(a, &b)
end
xs = ARGV.size > 5 ? "x" : [3, 1, 2]
p A.new(xs) { |x, y| y <=> x }.r
module Fac; def new(a) = super(a); end
class B < A; singleton_class.prepend(Fac); end
p B.new(xs).r
