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

# Passed directly: a literal and a local of the Integer Hash the analysis
# infers widen to the boxed-value Hash the parameter declares, the same
# hash, so the method's stores and the default reach the caller. One the
# binding cannot widen where it is built is rebuilt for the call.
module G
  #: (Hash[Integer, untyped]) -> Integer
  def self.count_iu(h) = h.size
  #: (Hash[Integer, untyped]) -> untyped
  def self.miss(h) = h[99]
  #: (Hash[Integer, untyped]) -> Integer
  def self.put(h)
    h[5] = "five"
    h.size
  end
  #: (Hash[Integer, Float]) -> Integer
  def self.count_if(h) = h.size
end

p G.count_iu({ 1 => 2 })
loc = { 3 => 4, 5 => 6 }
p G.count_iu(loc)
dflt = Hash.new(0)
dflt[7] = 1
p G.miss(dflt)
st = { 1 => 2 }
p G.put(st)
p st
p G.count_if({ 1 => 1.5 })
p G.count_iu([{ 8 => 9 }][0])
p G.count_iu({ 1 => 2, 3 => 4 }.select { |k, _| k > 1 })
