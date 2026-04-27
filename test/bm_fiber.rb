# Test Fiber (cooperative concurrency)
require "fiber"

# Basic yield/resume
f = Fiber.new {
  Fiber.yield(10)
  Fiber.yield(20)
  30
}
puts f.resume  # 10
puts f.resume  # 20
puts f.resume  # 30

# Value passing
f2 = Fiber.new { |first|
  second = Fiber.yield(first * 2)
  second * 3
}
puts f2.resume(5)   # 10
puts f2.resume(7)   # 21

# String passing
f3 = Fiber.new {
  Fiber.yield("hello")
  Fiber.yield("world")
  "done"
}
puts f3.resume  # hello
puts f3.resume  # world
puts f3.resume  # done

# alive?
f4 = Fiber.new {
  Fiber.yield(1)
  2
}
puts f4.alive?   # true
f4.resume
puts f4.alive?   # true
f4.resume
puts f4.alive?   # false

# Fiber.current
cur = Fiber.current
puts cur.alive?  # true

# FiberError on dead fiber
f5 = Fiber.new { 42 }
f5.resume
begin
  f5.resume
  puts "ERROR"
rescue FiberError
  puts "caught FiberError"
end
