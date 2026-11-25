# spinel: decisions
# Two adjacent in-place updates of a Float array in a loop: one range test
# ahead of both, and the same sums as one statement at a time.
def fill(h, bins, v)
  k = 0
  while k < bins.length
    s = bins[k] * 2
    h[s] += 1.0
    h[s + 1] += v
    k += 1
  end
end
h = Array.new(8, 0.0)
fill(h, [0, 1, 3, 1, 2, 0, 0], 0.5)
p h
