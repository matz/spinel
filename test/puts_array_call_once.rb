# puts of an Array a method answers evaluated the call once for the empty
# test and once per element read, so a method with a side effect ran once per
# element, and an empty Array answered by a call printed a blank line where
# CRuby prints nothing, as the literal `puts []` already did (#7197). The
# array is now read once into a temporary, for String, Integer and Float
# arrays alike, and an empty one prints nothing.
$calls = 0

def strs
  $calls += 1
  ["x", "y"]
end

def ints
  $calls += 1
  [1, 2, 3]
end

def floats
  $calls += 1
  [1.5, 2.5]
end

def none
  $calls += 1
  [].map { |v| v.to_s }
end

def no_ints
  $calls += 1
  [0].reject(&:zero?)
end

puts strs
puts ints
puts floats
puts none
puts no_ints
puts []
p $calls

with_nil = [1, nil, 3]
puts with_nil
local = %w[a b]
puts local
puts "done"
