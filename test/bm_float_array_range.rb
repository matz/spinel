# FloatArray slicing: a[range] and a[start, len].

a = [1.5, 2.5, 3.5, 4.5, 5.5]

# Range form
b = a[1..3]
puts b.length      # 3
puts b[0]          # 2.5
puts b[1]          # 3.5
puts b[2]          # 4.5

# (start, len) form
c = a[1, 2]
puts c.length      # 2
puts c[0]          # 2.5
puts c[1]          # 3.5

# Negative start
d = a[-2, 2]
puts d.length      # 2
puts d[0]          # 4.5
puts d[1]          # 5.5

# len exceeds remaining: clamped
f = a[2, 100]
puts f.length      # 3
puts f[0]          # 3.5
puts f[2]          # 5.5

# Bare a[i] still returns a float
puts a[0]          # 1.5
puts a[-1]         # 5.5

# Result is usable as a FloatArray
puts a[1..3].sum   # 10.5
