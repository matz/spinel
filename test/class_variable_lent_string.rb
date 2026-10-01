# A class variable handed to a method that appends to its parameter is the
# class's own String, as in CRuby: the method is lent the class variable's
# slot, as a global's or a top-level ivar's is. It was lent a copy in a
# temp, so the append never reached the class variable.

X = "x" * 40
def top(a) = (a << X; nil)

class C
  @@w = +"w"
  def m(a) = (a << X; nil)
  def n = (m(@@w); @@w.size)
  def t = (top(@@w); @@w.size)
  def self.k(a) = (a.concat("!"); nil)
  def self.go = (k(@@w); @@w.size)
end
class D < C; def d = (m(@@w); @@w.size); end
p C.new.n, C.new.t, C.go, D.new.d
