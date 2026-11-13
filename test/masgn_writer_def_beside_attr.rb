# A multiple assignment into `obj.x` stores through the writer the plan
# ranks first: an attr_accessor's field, or a `def x=` in the same class, an
# included module or a subclass, each as a call.
class SameClass
  attr_accessor :x
  def x=(v); @x = v * 10; end
end
a = SameClass.new
a.x, y = 5, 6
p a.x

class Mixed
  attr_accessor :x
end
m = Mixed.new
m.x = 1
class Mixed; def x=(v); @x = v * 10; end; end
m.x, y = 5, 6
p m.x

class Base
  attr_accessor :a, :b
end
class Sub < Base
  def a=(v); @a = v.to_s; end
end
s = Sub.new
s.a, s.b = 1, 2
p s.a, s.b
t = Base.new
t.a, t.b = 3, nil
p t.a, t.b

class Parent
  attr_accessor :x
end
module Writer
  def x=(v); @x = v.to_s; end
end
class Child < Parent
  include Writer
end
c = Child.new
c.x, y = 5, 6
p c.x
