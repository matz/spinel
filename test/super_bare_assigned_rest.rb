# A bare `super` after its method assigned its `*rest` passes the rest's
# current value, as CRuby does, where the method it reaches is the program's
# (a class above, a top-level def): it passed the parameter as it came. The
# method's block goes on with it.
class A
  def fmt(*a) = a.inspect
  def run(*a, &blk) = [a, blk ? blk.call : nil]
end

class B < A
  def fmt(*a); a = [9]; super; end
  def run(*a, &blk)
    a = a + [:x]
    super
  end
end
p B.new.fmt(1, 2)
p B.new.run(1, 2) { :blk }
p B.new.run(3)

class C < A
  def run(*a)
    a = [7]
    super
  end
end
p C.new.run(1) { :given }

class D < A
  def run(first, *rest, last)
    rest = [:r]
    super
  end
end
p D.new.run(1, 2, 3, 4)

def fmt(*a) = a.inspect
class Foo
  def fmt(*a); a = [9]; super; end
end
p Foo.new.fmt(1, 2)
