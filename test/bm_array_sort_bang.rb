# Array#sort! across typed-array variants.
# Previously only IntArray and SymArray dispatched; StrArray and
# FloatArray silently fell through.

ints = [3, 1, 4, 1, 5, 9, 2, 6]
ints.sort!
ints.each { |i| puts i }   # 1 1 2 3 4 5 6 9

floats = [3.5, 1.25, 4.75, 1.5, 0.25]
floats.sort!
floats.each { |f| puts f }  # 0.25 1.25 1.5 3.5 4.75

strs = ["banana", "apple", "cherry", "date"]
strs.sort!
strs.each { |s| puts s }    # apple banana cherry date

syms = [:c, :a, :d, :b]
syms.sort!
syms.each { |y| puts y }    # a b c d

# Single-element and empty edge cases
one = [99]
one.sort!
one.each { |i| puts i }     # 99

empty = []
empty.sort!
puts empty.length           # 0
