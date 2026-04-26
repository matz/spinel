# Test: built-in constructors via absolute ConstantPath (::X.new)

require "stringio"

a = ::Array.new(3, 2)
puts a[0] + a[2]

h = ::Hash.new
h["k"] = a[1]
puts h["k"]

p = ::Proc.new { |x| x + 1 }
puts p.call(5)

s = ::StringIO.new("ab")
puts s.getc

f = ::Fiber.new { |x|
  ::Fiber.yield(x + 1)
  x + 2
}
puts f.resume(4)
puts f.resume(4)

cur = ::Fiber.current
puts cur.alive?
