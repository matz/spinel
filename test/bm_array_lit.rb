# Test array literals as IntArray/StrArray

# Integer array literal
arr = [5, 3, 8, 1, 4]
puts arr.length    # 5
puts arr.size      # 5  (alias for length)
puts arr[0]        # 5
puts arr.sort[0]   # 1
puts arr.sum       # 21
puts arr.min       # 1
puts arr.max       # 8

# IntArray.size after mutation
arr.push(99)
puts arr.size      # 6  (length+1 after push)
arr.pop
puts arr.size      # 5  (length-1 after pop)

# Symbol array literal -- size shares the same dispatch as IntArray
syms = [:foo, :bar, :baz]
puts syms.length   # 3
puts syms.size     # 3

# String array literal
words = ["hello", "world", "foo"]
puts words.length  # 3
puts words.size    # 3
puts words[0]      # hello
puts words.join(", ") # hello, world, foo

# Empty array
empty = []
empty.push(42)
puts empty.length  # 1
puts empty.size    # 1
puts empty[0]      # 42

# Hoisted-length optimisation: .size should behave the same as
# .length when used as a loop bound. Both compile to a single
# read of the array's len field, hoisted out of the loop body.
sum = 0
i = 0
while i < arr.size
  sum += arr[i]
  i += 1
end
puts sum           # 21

ssum = 0
si = 0
while si < syms.size
  ssum += 1
  si += 1
end
puts ssum          # 3

wlen = 0
wi = 0
while wi < words.size
  wlen += words[wi].length
  wi += 1
end
puts wlen          # 13

puts "done"
