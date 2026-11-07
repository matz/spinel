# spinel: gc-stress
# A lambda inside a proc, lambda, Fiber or Thread block captures the block's
# optional, rest, post, keyword and keyword-rest parameters, as it captures a
# required one. Only requireds got a cell in the enclosing frame, so the
# others were refused at compile time ("proc referencing an uncaptured outer
# variable"). A block a call inlines still cells only its requireds.

# a proc's or lambda's parameters
pr = proc { |a, b = 2| inc = -> { b += 1 }; inc.call; inc.call; [a, b] }
p pr.call(1)
p pr.call(1, 10)
l = ->(a = 1, b = (a + 1)) { -> { [a, b] } }
p l.call.call
p l.call(5).call
pr = proc { |a, *r, z| -> { [a, r, z] } }
p pr.call(1, 2, 3, 4).call
pr = proc { |*r| -> { r.size } }
p pr.call(4, 5).call
pr = proc { |a, k: 1, **o| -> { [a, k, o] } }
p pr.call(1, k: 2, x: 3).call
p pr.call(1).call
# each call has cells of its own
ls = [1, 2, 3].map { |i| proc { |a, b = i| -> { b } }.call(0) }
p ls.map(&:call)
# two lambdas share the parameter's cell
g, s = proc { |a, b = 2| [-> { b }, -> { b += 10 }] }.call(1)
s.call
p g.call

# a Fiber's or Thread's
f = Fiber.new { |a, b = 2| set = -> { b = 7 }; set.call; Fiber.yield(b); -> { b }.call }
p f.resume(1)
p f.resume
f = Fiber.new { |a, *r| -> { r << 9; r }.call }
p f.resume(1, 2)
f = Fiber.new { |*r| -> { r.sum }.call }
p f.resume(1, 2, 3)
f = Fiber.new { |a, *r, z| -> { [r, z] }.call }
p f.resume(1, 2, 3, 4)
f = Fiber.new { |a, k: 2| -> { k * 10 }.call }
p f.resume(4, k: 3)
f = Fiber.new { |a, **kw| -> { kw }.call }
p f.resume(4, k: 3)
t = Thread.new(1, 2) { |a, b = 5, *r, k: 0| -> { [a, b, r, k] }.call }
p t.value
t = Thread.new(1, k: 4) { |a, k: 0| g = -> { k * a }; g.call }
p t.value

# a method's, as before
def mk(x, y = 3) = -> { x + y }
p mk(1).call
def mk2(*r, k: 1) = -> { [r, k] }
p mk2(1, 2, k: 5).call
