# A prepend a reopening runs after the class's first body, of a module a
# later reopening redefines the method of: a call runs the class's own
# method until the prepend has run, then the module's version it holds
# then, whose super reaches the class's method. Every guard arm and super
# target has a declaration.
module L
  def m = "L1 " + super
end
class K
  def m = "k"
end
k = K.new
def call(k) = k.m
p call(k)
class K; prepend L; end
p call(k)
module L
  def m = "L2 " + super
end
p call(k)
