# Integer-keyed Hashes in inline signatures. `Hash[Integer, Integer]` and
# `Hash[Integer, Float]` named no kind the RBS map had, and dropped the
# whole signature; `Hash[Integer, untyped]` fell to the bare poly, an
# sp_RbVal the analysis does not take for a Hash. They map to the Integer
# Hash and the boxed-value Hash now. A box handed to such a parameter from a
# branch that tested it (`v.is_a?(Hash) ? f(v) : v`, #613's shape) runs as
# before.
module F
  #: (Hash[Integer, untyped]) -> Integer
  def self.count_iu(h) = h.size
  #: (Hash[Integer, Integer]) -> Integer
  def self.sum_ii(h) = h.values.sum
  #: (Hash[Integer, Float]) -> Float
  def self.sum_if(h) = h.values.sum
end

vals = [{ 1 => "a", 2 => :b }, 3, { 5 => 6, 7 => 8 }, nil, { 9 => 1.5 }]
vals.each do |v|
  p(v.is_a?(Hash) ? F.count_iu(v) : v)
end
p F.sum_ii({ 1 => 10, 2 => 20 })
w = vals[2]
p(w.is_a?(Hash) ? F.sum_ii(w) : 0)
p F.sum_if({ 1 => 1.5, 2 => 2.25 })
h = {}
3.times { |i| h[i] = i * i }
p F.sum_ii(h)
