# A Method of a class method asked for before the method's def raises
# NameError, as CRuby's method lookup finds nothing there yet; once the
# def has run, the Method calls it. A Struct or Data class, a plain
# class, a nested one, and a subclass asking for its parent's; a capture
# in a lambda is placed where the lambda is called, and a lambda handed
# on keeps the def bound.

S = Struct.new(:a)
begin
  S.method(:z)
rescue NameError => e
  p e.class
  puts e.message
end
def S.z = 1
p S.method(:z).call

class P; end
m = (P.method(:z) rescue :none)
p m.is_a?(Method) ? :method : m
def P.z = 2
p P.z

module A
  class B; end
end
puts((A::B.method(:q) rescue $!.message))
class A::B
  def self.q = 3
end
p A::B.method(:q).call

class T < P; end
p((T.method(:w) rescue :none))
def P.w = 4
p T.method(:w).call

D = Data.define(:x)
def D.k = 5
def run_k = D.method(:k).call
p run_k

E = Data.define(:y)
$kept = []
def keep(f) = $kept << f
l = -> { E.method(:z).call }
m = l
keep(-> { E.method(:z).call })
def E.z = 6
p l.call
p m.call
p $kept[0].call
