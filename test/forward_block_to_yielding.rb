# `(...)` and `&b` hand the block to a yielding method on another receiver,
# a class of the program's or a reopened Hash's or Array's
class A
  def y = yield(1)
end
class B
  def initialize = @a = A.new
  def f(...) = @a.y(...)
  def g(&b) = @a.y(&b)
end
p B.new.f { |x| x + 1 }, B.new.g { |x| x + 2 }

class Hash
  def two = yield(2)
  def each_twice
    each { |k, v| yield k, v; yield k, v }
  end
end
class Array
  def three = yield(3)
end
class C
  def initialize = (@h = { "a" => 1 }; @a = [1])
  def g(&b) = @h.two(&b)
  def g2(...) = @h.two(...)
  def t(...) = @a.three(...)
  def twice(...) = @h.each_twice(...)
end
c = C.new
p c.g { |x| x * 10 }, c.g2 { |x| x * 10 }, c.t { |x| x * 10 }
c.twice { |k, v| p [k, v] }
{ "z" => 0 }.each_twice { |k, v| p [k, v] }

# a receiver that is sometimes nil: the call goes through the boxed
# dispatch, which hands the block on too (a literal block, or a lambda)
class Hash
  def add(n) = yield(n + size)
end
class Array
  def first_by = yield(self[0])
end
class W
  def initialize(h) = @h = h
  def f(...) = @h.two(...)
  def g(&b) = @h.two(&b)
  def a(n, &b) = @h.add(n, &b)
end
class V
  def initialize(x) = @x = x
  def f(...) = @x.first_by(...)
end
p W.new({}).f { |x| x + 1 }
p W.new({ k: 1 }).g { |x| x * 10 }
p W.new({ k: 1 }).a(5) { |x| x - 1 }
l = ->(x) { x * 3 }
p W.new({}).g(&l)
p((W.new(nil).f { |x| x } rescue $!.class))
p V.new([7, 8]).f { |x| x + 100 }
p((V.new(nil).f { |x| x } rescue $!.class))
