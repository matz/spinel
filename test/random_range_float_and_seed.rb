# Entropy-backed seeding, initial srand parity, degenerate-bound state
# preservation, and Float range validation / overflow-free scaling.

# Initial srand returns a positive non-zero seed, then tracks prior seeds.
prev = srand(42)
p [prev.is_a?(Integer), prev > 0]
p srand(7)
p srand
s1 = srand
s2 = srand
p [s1.is_a?(Integer), s1 > 0, s2.is_a?(Integer), s2 > 0, s1 != s2]

# Random.new_seed draws positive OS entropy without perturbing default stream.
ns1 = Random.new_seed
ns2 = Random.new_seed
p [ns1.is_a?(Integer), ns1 > 0, ns2 > 0, ns1 != ns2]
srand(123)
a = rand(100_000)
srand(123)
_ = Random.new_seed
b = rand(100_000)
p a == b

# Auto-seeded Random.new exposes a positive seed that reproduces its stream.
ra = Random.new
p [ra.seed.is_a?(Integer), ra.seed > 0, Random.new(ra.seed).rand == ra.rand]

# Degenerate bounds return the sole value without advancing generator state.
r = Random.new(9)
d = r.dup
p [r.rand(1), r == d]
p [r.rand(5..5), r == d]
p [r.rand(5...6), r == d]
p [r.rand(1.0..1.0), r == d]
p [r.rand(-0.0..-0.0).to_s, r.rand(-0.0..0.0).to_s, r == d]

# Overflow-free scaling on full Float::MAX spans (inclusive and exclusive).
r = Random.new(42)
ok_inc = true
ok_exc = true
ok_k = true
ok_c = true
100.times do
  x = r.rand(-Float::MAX..Float::MAX)
  ok_inc = false unless x.finite? && x >= -Float::MAX && x <= Float::MAX
  y = r.rand(-Float::MAX...Float::MAX)
  ok_exc = false unless y.finite? && y >= -Float::MAX && y < Float::MAX
  z = rand(-Float::MAX..Float::MAX)
  ok_k = false unless z.finite? && z >= -Float::MAX && z <= Float::MAX
  w = Random.rand(-Float::MAX...Float::MAX)
  ok_c = false unless w.finite? && w >= -Float::MAX && w < Float::MAX
end
p [ok_inc, ok_exc, ok_k, ok_c]

# Empty / reversed Float ranges: ArgumentError on Random, nil on Kernel#rand.
def check
  yield
rescue => e
  e.class
end

p check { r.rand(1.0...1.0) }
p check { r.rand(2.0..1.0) }
p check { r.rand(2.0...1.0) }
p check { r.rand(-0.0...0.0) }
p check { r.rand(2..1.5) }
p check { r.rand(2...2.0) }
p check { Random.rand(1.0...1.0) }
p check { Random.rand(2.0..1.0) }
p rand(1.0...1.0)
p rand(2.0..1.0)
p rand(2.0...1.0)
p rand(-0.0...0.0)
p rand(2..1.5)
p rand(2...2.0)

# Non-finite bounds (including +Inf..finite and finite..-Inf): Errno::EDOM.
p check { r.rand(0.0..Float::INFINITY) }
p check { r.rand(-Float::INFINITY..0.0) }
p check { r.rand(Float::INFINITY..0.0) }
p check { r.rand(0.0..-Float::INFINITY) }
p check { r.rand(Float::INFINITY..Float::INFINITY) }
p check { r.rand(1..Float::INFINITY) }
p check { r.rand(1..-Float::INFINITY) }
p check { Random.rand(Float::INFINITY..0.0) }
p check { Random.rand(0.0..-Float::INFINITY) }
p check { rand(0.0..Float::INFINITY) }
p check { rand(Float::INFINITY..0.0) }
p check { rand(0.0..-Float::INFINITY) }
p check { rand(1..-Float::INFINITY) }

# Boxed float ranges follow the same rules.
[1.0...1.0, 2.0..1.0, Float::INFINITY..0.0, 0.0..-Float::INFINITY, -Float::MAX..Float::MAX].each do |rg|
  kr = check { v = rand(rg); v.nil? ? nil : [v.class, v.finite?] }
  rr = check { v = r.rand(rg); [v.class, v.finite?] }
  p [kr, rr]
end

# Same-seed determinism and bucket coverage after switching to rejection sampling.
r1 = Random.new(42)
r2 = Random.new(42)
seq1 = Array.new(20) { r1.rand(6) }
seq2 = Array.new(20) { r2.rand(6) }
p seq1 == seq2
seen = {}
200.times { seen[r1.rand(1..6)] = true }
p seen.size == 6
p Random.new(3.9).rand(1000) == Random.new(3.9).rand(1000)
p Random.new(1e300).rand(1000) == Random.new(1e300).rand(1000)
