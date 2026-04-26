# FloatArray reductions: min/max/sum/first/last.
# Previously min/max returned 0 (no FloatArray dispatch — fell through).
# Use values whose float representation has a non-zero fractional part
# so Spinel's float-puts and CRuby's match (Spinel strips ".0" suffix).

f = [1.5, 2.5, 0.5, 3.5]
puts f.min       # 0.5
puts f.max       # 3.5
puts f.first     # 1.5
puts f.last      # 3.5

# Negative values
g = [-1.5, -3.25, 2.75]
puts g.min       # -3.25
puts g.max       # 2.75

# Single element
h = [4.5]
puts h.min       # 4.5
puts h.max       # 4.5
