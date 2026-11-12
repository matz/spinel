# A Method of a boxed receiver (an object of one of several classes) answers
# its #arity from the def its class had where the Method was made, as for a
# receiver of one class: a class that redefines the method afterwards does
# not change a Method made before. The classes' attribute readers and writers
# answer 0 and 1, a builtin receiver its own method's.

class A; def m(a) = a; end
class B; def m(a, b) = a; end
class R; attr_accessor :v; end
class W; def v = 1; def v=(x); @x = x; end; end

x = [A.new, B.new][ARGV.size]
mm = x.method(:m)
class A; def m(a, b, c) = a; end
class B; def m = 1; end
puts mm.arity
puts x.method(:m).arity
puts [A.new, B.new].map { |o| o.method(:m).arity }.inspect

y = [R.new, W.new][ARGV.size]
puts y.method(:v).arity, y.method(:v=).arity

z = [A.new, "ab", [1, 2]][ARGV.size + 1]
puts z.method(:size).arity

# a capture inside a method body or a loop is made at a point that may run
# before or after a redefinition: the Method holds the version that was live
def cap(o) = o.method(:n)
class A; def n(a) = a; end
class B; def n = 1; end
o = [A.new, B.new][ARGV.size]
held = cap(o)
puts held.arity
r = []
2.times { r << cap(o).arity; A.class_eval { def n(a, b) = a } }
puts r.inspect, held.arity, cap(o).arity

# an accessor the capture met stays the Method's after a def of the name
class C; attr_reader :w; end
class D; def w(a = 1) = 1; end
q = [C.new, D.new][ARGV.size]
mw = q.method(:w)
class C; def w(a, b) = 1; end
puts mw.arity, mw.source_location.last
