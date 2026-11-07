# spinel: gc-stress
# A Fiber or Thread block binds its keyword parameters from the keywords its
# first resume, or Thread.new, passes, as a proc call does. They were bound
# nil whatever was passed: `Thread.new(1, k: 2) { |a, k: 0| [a, k] }` gave
# [1, nil] for [1, 2], an optional keyword's default was never taken, and a
# missing required keyword raised nothing.
Thread.report_on_exception = false
def try = (yield rescue "#{$!.class}: #{$!.message}")

p Thread.new(1, k: 2) { |a, k: 0| [a, k] }.value
p Thread.new(1) { |a, k: 5| a + k }.value
p Thread.new(k: 2) { |k: 0| k * 3 }.value
p try { Thread.new(1, 2) { |a, b:| [a, b] }.value }
p try { Thread.new(1, z: 2) { |a, k: 0| [a, k] }.value }
arr = [1, 2]
p Thread.new(*arr, k: 3) { |a, b, k: 0| [a, b, k] }.value
threads = []
3.times { |i| threads << Thread.new(i, k: i * 10) { |a, k: 0| a + k } }
p threads.map(&:value)

f = Fiber.new { |a, k: 5| [a, k] }
p f.resume(1, k: 3)
f = Fiber.new { |a, k: 5| [a, k] }
p f.resume(1)
# a braced Hash is a positional value, not keywords
f = Fiber.new { |a, k: 5| [a, k] }
p f.resume(1, {k: 3})
f = Fiber.new { |a, k: 0| [a, k] }
p f.resume({k: 3})
f = Fiber.new { |a, k: 0| [a, k] }
p f.resume(k: 3)
f = Fiber.new { |a, k:, j:| [a, k, j] }
p try { f.resume(1) }
f = Fiber.new { |a, k: 0| [a, k] }
p try { f.resume(1, k: 2, y: 3, z: 4) }
# the keyword rest takes the keys no keyword names; a lone `**` any
f = Fiber.new { |a, **kw| [a, kw] }
p f.resume(1, k: 3, j: 4)
f = Fiber.new { |**kw| kw }
p f.resume
f = Fiber.new { |a, k: 0, **r| [a, k, r] }
p f.resume(1, "s" => 2, k: 3)
f = Fiber.new { |a, **| a }
p f.resume(1, x: 1)
h = {k: 7, j: 8}
f = Fiber.new { |k: 0, **r| [k, r] }
p f.resume(**h)
f = Fiber.new { |a, k: 0| [a, k] }
p f.resume(1, **{})
# with keywords one Array is not spread over the parameters; without, it is
f = Fiber.new { |a, b, k: 0| [a, b, k] }
p f.resume([1, 2], k: 3)
f = Fiber.new { |a, b, k: 0| [a, b, k] }
p f.resume([1, 2])
f = Fiber.new { |a, b, k: 0| [a, b, k] }
p f.resume(*arr, k: 3)
f = Fiber.new { |*r, k: 0| [r, k] }
p f.resume(1, 2, k: 3)
f = Fiber.new { |a, b = 4, k: 0| [a, b, k] }
p f.resume(1, k: 3)
f = Fiber.new { |(a, b), k: 1| [a, b, k] }
p f.resume([1, 2], k: 3)
# a default reads the parameters bound ahead of it
f = Fiber.new { |a, j: 1, k: (a + j)| [a, j, k] }
p f.resume(4, j: 2)
f = Fiber.new { |k: 1| k }
p f.transfer(k: 2)
# a block without keywords takes them as a Hash, as before
f = Fiber.new { |a, b| [a, b] }
p f.resume(1, k: 2)
# keywords bind on the first resume only; later ones hand over values
f = Fiber.new { |a, k: 0| x = Fiber.yield([a, k]); x }
p f.resume(1, k: 2)
p f.resume(7, k: 8)

class Meter
  def initialize = @v = 10
  def run
    f = Fiber.new { |a, k: (@v + a)| [a, k] }
    f.resume(1)
  end
  def run2 = Thread.new(2, k: 5) { |a, k: @v| [a, k, @v] }.value
end
p Meter.new.run
p Meter.new.run2
