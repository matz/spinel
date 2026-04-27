# Array#reverse! across typed-array variants.
# Previously only IntArray dispatched; SymArray, StrArray, FloatArray
# silently fell through.

ints = [1, 2, 3, 4, 5]
ints.reverse!
ints.each { |i| puts i }   # 5 4 3 2 1

floats = [1.5, 2.5, 3.5, 4.25]
floats.reverse!
floats.each { |f| puts f }  # 4.25 3.5 2.5 1.5

strs = ["alpha", "beta", "gamma"]
strs.reverse!
strs.each { |s| puts s }    # gamma beta alpha

syms = [:a, :b, :c, :d]
syms.reverse!
syms.each { |y| puts y }    # d c b a

# Even-length and single-element edge cases
even = [10, 20]
even.reverse!
even.each { |i| puts i }   # 20 10

one = [42]
one.reverse!
one.each { |i| puts i }    # 42

empty = []
empty.reverse!
puts empty.length          # 0
