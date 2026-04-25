# Test integer-keyed string-valued hash (sp_IntStrHash)
# Regression: previously segfaulted — codegen emitted sp_StrStrHash,
# causing sp_str_hash() to dereference integer keys as pointers.

# Literal creation and lookup
h = {3 => "Fizz", 5 => "Buzz", 7 => "Bazz"}
puts h[3]          # Fizz
puts h[5]          # Buzz
puts h[7]          # Bazz
puts h[1]          # (empty)

# has_key?
puts h.has_key?(3)   # true
puts h.has_key?(4)   # false

# length
puts h.length        # 3

# keys returns int array
puts h.keys.length   # 3
puts h.keys[0]       # 3

# values
puts h.values[0]     # Fizz

# keys.each iteration
h.keys.each do |k|
  puts k
end

# each iteration (key, value)
h.each do |k, v|
  puts "#{k}:#{v}"
end

# []= assignment
h[15] = "FizzBuzz"
puts h[15]           # FizzBuzz
puts h.length        # 4

# fetch with default
puts h.fetch(3, "none")   # Fizz
puts h.fetch(99, "none")  # none

# FizzBuzz pattern — the original failing case
map = {3 => "Fizz", 5 => "Buzz"}
(1..15).each do |x|
  out = ""
  map.keys.each do |k|
    out += map[k] if x % k == 0
  end
  out = x.to_s if out.empty?
  puts out
end
