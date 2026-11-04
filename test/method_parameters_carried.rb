# A Method carries its parameter list: one read back out of a container,
# or one of several a variable may hold, answers #parameters as the Method
# named in place does, anonymous parameters (`*`, `**`, `&`) and a
# parameter named `_` included.
class K
  def a(x, y = 1, *r, z, k:, o: 2, **kw, &blk) = nil
  def b(*, **, &) = nil
  def u(_, _a) = nil
  def self.s(q) = q
end
k = K.new

ms = [k.method(:a), k.method(:b), k.method(:u), K.method(:s)]
ms.each { |m| p m.parameters }

h = { b: k.method(:b), u: k.method(:u) }
p h[:b].parameters, h[:u].parameters
p h[:b].unbind.parameters

# one of two Methods, chosen at run time
m = ARGV.empty? ? k.method(:b) : k.method(:u)
p m.parameters

um = K.instance_method(:b)
p [um][0].parameters, um.bind(k).parameters

# a define_method's Method: the block's parameters, by the names written
# (a trailing comma adds none; a destructuring group has no name)
class D
  define_method(:dn) { |x| x }
  define_method(:d2) { |x, y = 1| x }
  define_method(:dc) { |x,| x }
  define_method(:dd) { |a, (b, c)| a }
  define_method(:dr) { |x, *| x }
  def dp((x, y), z) = z
end
d = D.new
%i[dn d2 dc dd dr dp].each { |s| p d.method(s).parameters }
p [d.method(:dn)][0].parameters, [d.method(:d2)][0].parameters, [d.method(:dd)][0].parameters
p [D.instance_method(:dn)][0].parameters, D.instance_method(:d2).parameters
p [d.method(:dp)][0].parameters

# a long name survives the list
class L
  def a(xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1) = 1
end
p [L.new.method(:a)][0].parameters == L.new.method(:a).parameters

# a name the program writes is its own, even one shaped like a name the
# compiler gives a parameter it makes or names
class U
  def a(__destr_q, __implicit_rest_z) = 1
  def b(*__anon_rest) = 1
  define_method(:c) { |__blk_kwrest1| 1 }
  define_method(:e) { |x, **__blk_kwrest2| 1 }
end
uo = U.new
%i[a b c e].each { |s| p uo.method(s).parameters, [uo.method(s)][0].parameters }
p proc { |__destr_q, __implicit_rest_z| }.parameters, proc { |x, **__blk_kwrest3| }.parameters
p proc { |__destr_q, __implicit_rest_z| }.arity, proc { |x,| }.arity

# a method that yields takes its block without naming it
def decide(event) = yield(event)
p [method(:decide)][0].parameters, [method(:decide)][0].call(4) { |e| e * 2 }
