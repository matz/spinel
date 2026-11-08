# A bare call in a block, lambda, proc, Fiber or Enumerator body runs a
# subclass's own method before a top-level def or Object's method, as
# the same call outside a block does.
def hi = "top"
def hey(x = 1) = "top#{x}"
class Object; def greet = "obj"; end
class Other; def hi = "other"; end
class Foo
  def each_blk = [1, 2].each { |i| print hi, i, " " }
  def times_blk = 2.times.map { hi }
  def nested = [1].map { [2].map { hi } }
  def lam = -> { hi }
  def pr = proc { hey(5) }
  def objm = [1].map { greet }
  def selfm = [1].map { self.greet }
  def ie = Other.new.instance_exec { hi }
  def ie2 = Other.new.instance_eval { [1].map { hi } }
  def fib = Fiber.new { Fiber.yield hi; :done }.resume
  def enum = Enumerator.new { |y| y << hi }.to_a
  def ret_blk = [1].each { return hi }
  def with_args = [3].map { |x| hey(x) }
  def blk_arg(&b) = b.call
  def pass_blk = blk_arg { hi }
  define_method(:dm) { hi }
  def self.cm = [1].map { hi }
end
class Sub < Foo; def hi = "sub"; def hey(x = 2) = "sub#{x}"; def greet = "subg"; end
class Sub2 < Foo; end
s = Sub.new
s.each_blk; puts
p s.times_blk, s.nested, s.lam.call, s.pr.call, s.objm, s.selfm, s.ie, s.ie2, s.fib, s.enum, s.ret_blk, s.with_args, s.pass_blk, s.dm, Foo.cm, Sub.cm
f = Foo.new
p f.times_blk, f.lam.call, f.pr.call, f.objm, f.with_args, f.dm
p Sub2.new.times_blk, Sub2.new.lam.call
l = s.lam
p [l.call, Foo.new.lam.call]
p [1].map { hi }
