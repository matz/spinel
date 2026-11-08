# A call the program runs ahead of a builtin class's reopening of the
# method's name, at its top level, runs the builtin's method, as in CRuby;
# one after the reopening, or in a method called later, runs the
# reopening's (#10719).
s = "abc"
a = [3, 1]
h = {a: 1}
p s.upcase, a.sum, a.first, h.size, :sym.length
class String
  def upcase = "UP"
end
class Array
  def sum = 99
  def first = :mine
end
class Hash
  def size = -1
end
class Symbol
  def length = 42
end
p s.upcase, a.sum, a.first, h.size, :sym.length
def later(s, a, h) = [s.upcase, a.sum, h.size]
p later("x", [1], {})
