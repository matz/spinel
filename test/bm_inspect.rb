# Primitives: .inspect as a method call

# Integer
puts 5.inspect
puts (-42).inspect
puts 0.inspect

# Float
puts 1.5.inspect
puts 1.0.inspect
puts (-3.25).inspect

# String
puts "hi".inspect
puts "".inspect
puts "a\nb".inspect
puts "tab\there".inspect
puts "quote\"inside".inspect
puts "back\\slash".inspect

# Symbol (regression)
puts :foo.inspect

# Boolean
puts true.inspect
puts false.inspect

# nil
puts nil.inspect

# Interpolation via explicit .inspect call
x = 42
puts "got #{x.inspect}"

# Kernel#p equivalence: p obj == puts(obj.inspect) for scalars
p 5
p 1.0
p "hi"
p :foo
p true
p nil

# Arrays: .inspect + p
p []
p [1, 2, 3]
p [-5, 0, 42]
p [99]
p [1.5, 2.0]
p [1.0]
p ["hello", "world"]
p [""]
p [:foo, :bar]

# Heterogeneous / mixed-type arrays (poly_array)
p [1, "x"]
p [nil, :sym, 1.5]
p [1, "two", :three, 4.0, true]
p [false, nil]

# Mixed arrays as a receiver (inspect / to_s as methods)
mx = [1, "x"]
puts mx.inspect
puts mx.to_s

# Array#inspect as a method call
arr = [1, 2, 3]
puts arr.inspect

# Array#to_s is aliased to Array#inspect in CRuby
puts arr.to_s

# Interpolation uses to_s
puts "got #{arr.inspect}"

# From a method returning an array
def make_arr; [10, 20, 30]; end
p make_arr

# puts on arrays: flatten, element-per-line (not inspect)
puts [1, 2, 3]
puts []
puts ["a", "b"]
puts [:x, :y]
puts [1.5, 2.0]

puts "done"
