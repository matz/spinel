# Test String#ljust, rjust, center with custom pad string

# center with default (space) pad
puts "hi".center(10)        #     hi
puts "hi".center(9)         #    hi

# center with single-char pad
puts "hi".center(10, "-")   # ----hi----
puts "hi".center(9,  "-")   # ---hi----

# center with multi-char pad (cycling)
puts "hi".center(10, "ab")  # ababhiabab
puts "hi".center(9,  "ab")  # abahiabab

# ljust with multi-char pad (cycling)
puts "hi".ljust(8, "ab")    # hiababab
puts "hi".ljust(8, "xyz")   # hixyzxyz

# rjust with multi-char pad (cycling)
puts "hi".rjust(8, "ab")    # abababhi
puts "hi".rjust(8, "xyz")   # xyzxyzhi

# no-op when string already long enough
puts "hello".ljust(3, "x")  # hello
puts "hello".rjust(3, "x")  # hello
puts "hello".center(3, "x") # hello

puts "done"
