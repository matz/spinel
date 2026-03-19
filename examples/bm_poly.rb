# Test polymorphic variables (sp_RbValue)

# Variable holding different types
x = 1
puts x      # 1
x = "hello"
puts x      # hello

# Polymorphic method parameter
def show(val)
  puts val
end
show(42)
show("world")
show(true)
show(3.14)

# Nilable
y = "found"
puts y      # found
y = nil
puts y.nil? # true — but this needs nil tracking

puts "done"
