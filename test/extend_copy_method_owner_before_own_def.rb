# A Method of an extended method taken before the class's own def shadows
# it answers the module as its owner, as an include's does behind an own
# instance def; a local later holding the own def's Method answers that.
module V; def v = 1; end
class C; extend V; end
m = C.method(:v)
p m.owner
p m.call
p m.inspect.include?("#<Class:C>(V)#v()")
p m.source_location[1]
def C.v = 2
p C.method(:v).owner
p C.v
m = C.method(:v)
p m.owner
p m.source_location[1]

module W; def w = 3; end
class D; include W; end
o = D.new
n = o.method(:w)
class D; def w = super + 10; end
p n.owner
p n.call
p n.inspect.include?("D(W)#w()")
p o.w
