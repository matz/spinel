# spinel: int64
# Full 64-bit integer range bounds in Random#rand, Random.rand, and Kernel#rand:
# exercises INT64_MIN exclusive underflow, INT64_MAX - INT64_MIN overflow, and
# full UINT64_MAX span without +1 wrap-to-zero.

imax = ((1 << 62) - 1) + (1 << 62)
imin = -imax - 1

def check
  yield
rescue => e
  e.class
end

r = Random.new(42)
p check { r.rand(imin...imin) }
p check { Random.rand(imin...imin) }
p rand(imin...imin)

d = r.dup
p [r.rand(imin..imin) == imin, r == d]
p [r.rand(imax..imax) == imax, r == d]

ok_full = true
saw_neg = false
saw_pos = false
100.times do
  x = r.rand(imin..imax)
  ok_full = false unless x.is_a?(Integer) && x >= imin && x <= imax
  saw_neg = true if x < 0
  saw_pos = true if x >= 0
end
p [ok_full, saw_neg, saw_pos]

ok_excl = true
100.times do
  x = r.rand(imin...imax)
  ok_excl = false unless x.is_a?(Integer) && x >= imin && x < imax
end
p ok_excl

ok_half = true
50.times do
  a = r.rand(imin..0)
  b = r.rand(0..imax)
  ok_half = false unless a >= imin && a <= 0 && b >= 0 && b <= imax
end
p ok_half

# Kernel#rand and Random.rand on full-span ranges.
kx = rand(imin..imax)
ke = rand(imin...imax)
cx = Random.rand(imin..imax)
p [kx.is_a?(Integer) && kx >= imin && kx <= imax,
   ke.is_a?(Integer) && ke >= imin && ke < imax,
   cx.is_a?(Integer) && cx >= imin && cx <= imax]

# Boxed range arguments.
bx = [imin..imax, 0][0]
be = [imin...imin, 0][0]
v1 = r.rand(bx)
v2 = rand(bx)
p [v1.is_a?(Integer) && v1 >= imin && v1 <= imax,
   v2.is_a?(Integer) && v2 >= imin && v2 <= imax,
   rand(be),
   check { r.rand(be) }]

# Same-seed reproducibility on full-span draws.
p Random.new(77).rand(imin..imax) == Random.new(77).rand(imin..imax)
p Random.new(77).rand(imin...imax) == Random.new(77).rand(imin...imax)
