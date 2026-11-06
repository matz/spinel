# Method#to_proc's parameters are the Method's: a lambda's, whose
# required positionals are optional in the proc view. A method with no Ruby
# body (a builtin written in C, an accessor, a Struct member) lists the
# unnamed parameters of its arity instead, in either view.

class K
  def a(x, y = 1, *r, z, k: 1, j:, **o, &b) = nil
  def kw(a: 1, b:) = nil
  def nokw(x, **nil) = x
  def s(*) = nil
  def f(...) = nil
  define_method(:d) { |x, y = 2| x }
  attr_accessor :v
  def keep(&b) = b
  def g(x, (y, z), w = 1) = x
end
k = K.new
p k.method(:a).to_proc.parameters
p k.method(:a).to_proc.parameters(lambda: false)
p k.method(:a).to_proc.parameters(lambda: true)
p k.method(:kw).to_proc.parameters
p k.method(:nokw).to_proc.parameters(lambda: false)
p k.method(:s).to_proc.parameters
p k.method(:f).to_proc.parameters
p k.method(:d).to_proc.parameters(lambda: false)
p k.method(:v).to_proc.parameters
p k.method(:v=).to_proc.parameters(lambda: false)
p k.method(:g).to_proc.parameters
p k.method(:g).to_proc.parameters(lambda: false)
S = Struct.new(:m)
st = S.new(1)
p [st.method(:m=).parameters, st.method(:m=).to_proc.parameters, st.method(:m=).to_proc.parameters(lambda: false)]
p st.method(:m).to_proc.parameters

def top(a, *b) = a
p method(:top).to_proc.parameters
p method(:Integer).to_proc.parameters(lambda: false)
p 1.method(:+).to_proc.parameters(lambda: false)
p "s".method(:center).to_proc.parameters
p [1].method(:first).to_proc.parameters(lambda: false)
p 1.method(:between?).to_proc.parameters(lambda: false)
p 1.method(:clamp).to_proc.parameters
ar = [1, 2]
p [ar.method(:push).to_proc.parameters, ar.method(:[]=).to_proc.parameters(lambda: false)]

# through a block parameter, an ivar, an Array and a Hash
pr = k.keep(&k.method(:kw))
p [pr.lambda?, pr.parameters]
class H
  def initialize(m) = @p = m.to_proc
  def params = @p.parameters
end
p H.new(k.method(:nokw)).params
ms = [k.method(:a), 1.method(:+)]
p ms.map { |m| m.to_proc.parameters }
h = { f: k.method(:d).to_proc, g: ->(q) { q } }
p h.transform_values(&:parameters)
p h.transform_values { |v| v.parameters(lambda: false) }
