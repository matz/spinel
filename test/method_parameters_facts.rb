# Method#parameters answers CRuby's list: keywords required first, **nil,
# accessors (attr and Struct members), and builtins written in C or in Ruby

class K
  def a(x, y = 1, *r, z, k: 1, j:, **o, &b) = nil
  def kw(a: 1, b:, c: 3, d:) = nil
  def nokw(**nil) = nil
  attr_accessor :v
  attr_writer :w
end
k = K.new
p k.method(:a).parameters
p k.method(:kw).parameters
p K.instance_method(:kw).parameters
p k.method(:nokw).parameters
p k.method(:v).parameters
p k.method(:v=).parameters
p k.method(:w=).parameters

S = Struct.new(:m) { attr_accessor :extra }
class S
  attr_writer :b
end
class Sub < Struct.new(:q)
  attr_writer :zz
end
p S.new(1).method(:m).parameters
p S.new(1).method(:m=).parameters
p S.new(1).method(:extra=).parameters
p S.new(1).method(:b=).parameters
p Sub.new(1).method(:q=).parameters
p Sub.new(1).method(:zz=).parameters

module Mx
  attr_accessor :z
end
class C
  include Mx
end
p C.new.method(:z=).parameters

# builtins written in C
p 1.method(:+).parameters
p "s".method(:center).parameters
p "s".method(:length).parameters
p({a: 1}.method(:fetch).parameters)
p 1.5.method(:round).parameters
p 3.method(:between?).parameters
p 3.method(:clamp).parameters

# builtins written in Ruby, and Kernel's
p 1.method(:times).parameters
p [1].method(:first).parameters
p "s".method(:then).parameters
p method(:puts).parameters
p method(:Integer).parameters

# Methods out of containers and through a method argument
def params_of(m) = m.parameters
p params_of(1.method(:+))
ms = [k.method(:kw), k.method(:v=), 1.5.method(:round), "s".method(:center)]
p ms.map(&:parameters)
h = { f: method(:Float), u: "s".method(:upcase), r: k.method(:v) }
p h.transform_values(&:parameters)
