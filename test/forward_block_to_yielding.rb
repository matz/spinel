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
