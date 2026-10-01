# `obj.x = v` used as a value, where a hand-written `def x=` overrides an
# attr_writer whose slot is boxed: the value is the argument as written,
# which the emitter hands back as is, but the inference typed it from the
# attribute's boxed slot. The two disagreed and the C did not build: the
# slot holds two kinds here, and under --int-overflow=promote every
# Integer slot is boxed.
class B
  attr_writer :x
  def x_value = @x
end

class A < B
  def x=(v)
    super
  end
end

b = B.new
b.x = "s"
a = A.new
r = (a.x = 6)
p r, a.x_value, b.x_value
q = (a.x = "t")
p q

class C
  attr_writer :y
  def y_value = @y
  def y=(v)
    @y = v * 2
  end
end
c = C.new
t = (c.y = 5)
p t, c.y_value
