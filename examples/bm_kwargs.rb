# Test keyword arguments and splat

def greet(name:, greeting: "Hello")
  puts greeting
  puts name
end

greet(name: "world")
greet(name: "Ruby", greeting: "Hi")

# Rest args (splat)
def sum(*nums)
  total = 0
  nums.each do |n|
    total += n
  end
  total
end

puts sum(1, 2, 3)    # 6
puts sum(10, 20)     # 30
