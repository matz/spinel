# Test format/sprintf
puts format("%d:%02d", 5, 3)
puts sprintf("hello %s %d", "world", 42)

# Test inline rescue as expression
x = raise("oops") rescue 42
puts x

# Test symbol key hash (string values -> sp_RbHash)
h = {running: "green", waiting: "yellow"}
puts h[:running]
puts h[:waiting]

# Test symbol key hash with integer values (-> sp_StrIntHash)
h2 = {a: 1, b: 2}
puts h2[:a]
puts h2[:b]
