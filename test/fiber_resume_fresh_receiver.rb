# spinel: gc-stress
# A fiber nothing else holds, resumed or transferred to with several values in
# one expression: the values are packed after it is made, which allocates, and
# a collection there freed it (SPINEL_GC_STRESS=2 raised "fiber called across
# threads"). Such a receiver is now held in a rooted temp for the call.

def make = Fiber.new { |a, b| [a, b] }
p make.resume(1, 2)
p Fiber.new { |a, b| a + b }.resume(3, 4)
p Fiber.new { |*r| r }.resume(*[5, 6, 7])
p Fiber.new { |a| a }.resume("x" * 3)
p [Fiber.new { |a, b| a * b }][0].resume(6, 7)
f1 = Fiber.new { |a, b| Fiber.yield(a + b); :done }
p f1.resume(1, 2), f1.resume
p((1..20).map { |i| Fiber.new { |a, b| a * b }.resume(i, i + 1) }.sum)

# transfer
p Fiber.new { |a, b| a + b }.transfer(1, 2)

# a fiber a local holds, as before
f2 = Fiber.new { |a, b| a * b }
p f2.resume(3, 4)
