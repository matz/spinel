# A Fiber or Thread block binds an optional parameter, a rest beside other
# parameters, and a post parameter from what it is handed, as a proc call
# does. Only the requireds, or a lone rest, were bound: the others were read
# from the enclosing scope's slot of the same name, nil, so `|a, b = 7|`
# gave b nil for 7 and `|a, *r|` gave r nil.

# (a fiber built and resumed in one expression is held by nothing while
# several values are packed for it: each one goes through resume_with, whose
# parameter holds it)
def resume_with(f, *a) = f.resume(*a)

p resume_with(Fiber.new { |a, b = 7| [a, b] }, 1)
p resume_with(Fiber.new { |a, b = 7| [a, b] }, 1, 2)
p resume_with(Fiber.new { |a, *r| [a, r] }, 1, 2, 3)
p resume_with(Fiber.new { |a, *r| [a, r] })
p resume_with(Fiber.new { |*r, z| [r, z] }, 1, 2, 3)
p resume_with(Fiber.new { |a = 3, *r, z| [a, r, z] }, 9)

# posts are served before optionals, and the rest takes the middle
p resume_with(Fiber.new { |a, b = 7, c = 8, *r, y, z| [a, b, c, r, y, z] }, 1, 2, 3)
p resume_with(Fiber.new { |a, b = 7, c = 8, *r, y, z| [a, b, c, r, y, z] }, 1, 2, 3, 4, 5, 6, 7)

# one Array spreads over several slots, not over a lone optional
p resume_with(Fiber.new { |a, b = 7| [a, b] }, [1, 2])
p resume_with(Fiber.new { |*r, z| [r, z] }, [1, 2])
p resume_with(Fiber.new { |b = 7| b }, [1, 2])
p resume_with(Fiber.new { |b = 7| b }, 1, 2)
p resume_with(Fiber.new { |b = 7| b })

# a default that reads an earlier parameter, and one written in the body
p resume_with(Fiber.new { |a, b = (a * 2)| [a, b] }, 4)
p resume_with(Fiber.new { |a, b = 7| b += 1; [a, b] }, 1)
p resume_with(Fiber.new { |a, b = 7| [a, b] }, nil)

# defaults that read an outer local, an ivar, a method, run a block, build
# a String, and run only when no value reaches them
outer = 40
p resume_with(Fiber.new { |a, b = (outer + 2)| [a, b] }, 1)
class Box
  def initialize = (@base = 100)
  def helper = 5
  def go = resume_with(Fiber.new { |a, b = @base, c = helper| [a, b, c] }, 1)
end
p Box.new.go
p resume_with(Fiber.new { |a, b = [1, 2].map { |x| x * a }| [a, b] }, 3)
p resume_with(Fiber.new { |a, s = "x#{a}"| s + "!" }, 4)
$runs = 0
def bump = ($runs += 1)
p resume_with(Fiber.new { |a, b = bump| [a, b] }, 1, 2), $runs
p resume_with(Fiber.new { |a, b = bump| [a, b] }, 1), $runs

# a fiber resumed again keeps its parameters
g = Fiber.new { |x, y = 5|
  z = Fiber.yield(x + y)
  [x, y, z]
}
p g.resume(1)
p g.resume(:again)

# Thread.new, with and without a splat
p Thread.new(1, 2, 3) { |a, *r, z| [a, r, z] }.value
p Thread.new(1) { |a, b = 7| [a, b] }.value
p Thread.new(*[1, 2, 3]) { |a, *r, z| [a, r, z] }.value
p Thread.new([5, 6]) { |a, b = 1| [a, b] }.value
p (1..3).map { |i| Thread.new(i, *[i * 2]) { |a, b = 0, *rest| a + b + rest.size } }.map(&:value)

# requireds only, and a lone rest, bind as before
p Thread.new(1, 2) { |a, b| a + b }.value
p resume_with(Fiber.new { |*r| r }, 1, 2)
