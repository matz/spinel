# Array.new(N) { |i| ... }: block form with index parameter.
# Previously returned an empty IntArray (the block was ignored).

# Block uses index
a = Array.new(5) { |i| i * 2 }
puts a.length      # 5
puts a[0]          # 0
puts a[2]          # 4
puts a[4]          # 8
puts a.sum         # 20

# Block returns constant
b = Array.new(3) { 42 }
puts b.length      # 3
puts b[0]          # 42
puts b[2]          # 42

# Zero-length
c = Array.new(0) { 99 }
puts c.length      # 0
