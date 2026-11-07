# >=, <, between? and clamp on a boxed value whose class has no Comparable
# raise NoMethodError, as CRuby does (not Comparable's ArgumentError). A
# program exception class that mixes in Comparable still compares.
def ge(x) = x >= 0
def lt(x) = x < 0
def err(e) = [e.class, e.message.sub(/0x\h+/, "X")]
vals = [nil, 1, [1], true, false, :s, (1..2), proc { 1 }, /a/,
        RuntimeError.new("x"), "s", 1.5, Object.new]
vals.each do |v|
  p((ge(v) rescue err($!)))
  p((lt(v) rescue err($!)))
  p((v.between?(0, 1) rescue err($!)))
  p((v.clamp(0, 1) rescue err($!)))
end

module M
  class E < StandardError; include Comparable; def <=>(o) = 0; end
end
class F < M::E; end
module Cmp; include Comparable; end
class G < RuntimeError; include Cmp; def <=>(o) = -1; end
class H < RuntimeError; def <=>(o) = 0; end
class K < ArgumentError; def >=(o) = :k; end
[M::E.new, F.new, G.new, H.new, K.new, RuntimeError.new].each do |v|
  p((ge(v) rescue err($!)))
  p((v.between?(0, 1) rescue $!.class))
end
