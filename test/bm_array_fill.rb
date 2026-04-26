# Array#fill: 1-arg, 2-arg, 3-arg forms.
# Previously the 2-/3-arg forms silently ignored start/length and
# filled the entire array.

# 1-arg: fill all
a = [1, 2, 3, 4, 5]
a.fill(9)
puts a[0]    # 9
puts a[4]    # 9

# 2-arg: fill from start to end
b = [1, 2, 3, 4, 5]
b.fill(9, 2)
puts b[0]    # 1
puts b[1]    # 2
puts b[2]    # 9
puts b[4]    # 9

# 3-arg: fill from start, length elements
c = [1, 2, 3, 4, 5]
c.fill(0, 1, 3)
puts c[0]    # 1
puts c[1]    # 0
puts c[3]    # 0
puts c[4]    # 5

# Negative start
d = [1, 2, 3, 4, 5]
d.fill(7, -2)
puts d[0]    # 1
puts d[2]    # 3
puts d[3]    # 7
puts d[4]    # 7

# 3-arg with start beyond length: array grows.
# CRuby fills the gap with nil; Spinel's IntArray can't hold nil so it
# uses 0. The grown length and the explicit fill values are the same;
# we only assert on those (skip e[3]/e[4] which differ in formatting).
e = [1, 2, 3]
e.fill(9, 5, 2)
puts e.length   # 7
puts e[2]       # 3
puts e[5]       # 9
puts e[6]       # 9

# 2-arg with start beyond length: no-op, array unchanged.
f = [1, 2, 3]
f.fill(9, 5)
puts f.length   # 3
puts f[2]       # 3

# Very-negative start: clamped to 0 after wrap (start + len < 0).
# CRuby: [1,2,3].fill(9, -5, 2) #=> [9, 9, 3]
g = [1, 2, 3]
g.fill(9, -5, 2)
puts g[0]       # 9
puts g[1]       # 9
puts g[2]       # 3
