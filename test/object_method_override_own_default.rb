# A builtin class's override of a program's Object method takes its own
# parameter defaults, not Object's.
$n = 0
class Object
  def o(x = 1) = [:obj, x]
  def kw(a, k: 1) = [:obj, a, k]
  def same(x = 6) = [:obj, x]
  def dd(a, b = a + 1, c = self.class.name) = [:obj, a, b, c]
  def ee(x = "s") = [:obj, x]
  def ff(x = ($n += 1)) = [:obj, x, $n]
end
class FalseClass
  def o(x = 2) = [:false, x + 1]
  def kw(a, k: 1) = [:false, a, k]
  def same(x = 6) = [:false, x]
end
class TrueClass
  def o(x = 1) = [:true, x]
  def ee(x = :t) = [:true, x]
end
class Hash
  def o(x = 3) = [:hash, x]
  def dd(a, b = a * 10, c = size) = [:hash, a, b, c]
  def ee(x = 5) = [:hash, x]
  def ff(x = ($n += 100)) = [:hash, x, $n]
end
class Array
  def kw(a, k: 2) = [:ary, a, k]
  def same(x = 6) = [:ary, x]
end
class Proc
  def o(x = 4) = [:proc, x]
end
class Rational
  def kw(a, k: 5) = [:rational, a, k]
end

p false.o
p false.o(5)
p true.o
p({}.o)
p 7.o
p proc {}.o
p [].kw(0)
p [].kw(0, k: 9)
p false.kw(0)
p 2r.kw(0)
p false.same
p [].same

def g(v) = v.o
def h(v) = v.kw(0)
def s(v) = v.same
def t(v) = [v.dd(2), v.ee, v.ff]
[false, true, {}, 7, "s", proc {}].each { |v| p g(v) }
[false, [1], 7, 3r].each { |v| p h(v) }
[false, [1], 7].each { |v| p s(v) }
p t({a: 1}), t(true), t(3), t([1])
