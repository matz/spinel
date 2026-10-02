# A String variable on this route must not silently lose its append.
s = +"a"
f = Fiber.new { |t| t << "!" }
f.resume(s)
p s
