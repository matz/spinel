# An instance's `send` whose whole body is `super(*a)` (or a name, a splat and
# a block, or `...`) hands its parameters to Object#send as they came: it is
# no send of the program's own, so a send on an instance runs the named method.

class A
  def send(*a) = super(*a)
  def m = :a
end
p A.new.send(:m)

class B
  def __send__(name, *args, &blk) = super(name, *args, &blk)
  def m(x) = [:b, x]
end
p B.new.__send__(:m, 4)

class C
  def public_send(...) = super(...)
  def m = :c
end
p C.new.public_send(:m)

class Plain
  def m = :plain
end
v = [A.new, Plain.new][ARGV.size]
p v.send(:m)

class D
  def send(name, *a, **kw, &b) = super(name, *a, **kw, &b)
  def m(*a, **kw) = [:d, a, kw]
end
p D.new.send(:m, 1, a: 2)
p D.new.send(:m)
