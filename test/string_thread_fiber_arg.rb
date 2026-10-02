# Literal arguments and read-only String parameters retain their behaviour.
s = +"a"
p Thread.new(s) { |t| t + "!" }.value
p s
p Thread.new(+"b") { |t| t << "!" }.value
f = Fiber.new { |t| t << "?" }
p f.resume(+"c")
a = [1]
Thread.new(a) { |t| t << 2 }.join
p a
# A local read only as the argument cannot observe the copy.
fiber_arg = +"d"
f = Fiber.new { |t| t << "?" }
p f.resume(fiber_arg)
