# Two Methods compared as boxed values (a wrapper that holds its subject in an
# ivar and receives the other as an untyped argument, as an mspec-style
# `x.should.eq(y)` does) are equal exactly when the typed == says so: the same
# method of the same receiver, whichever of its names made them.
class Expect
  def initialize(v)
    @v = v
  end
  def eq(o)
    puts(@v == o)
    puts(@v != o)
  end
end
class Object
  def should
    Expect.new(self)
  end
end
a = [1, 2]
true.method(:to_s).should.eq(true.method(:to_s))
true.method(:inspect).should.eq(true.method(:to_s))
false.method(:^).should.eq(false.method(:|))
a.method(:size).should.eq(a.method(:length))
a.method(:size).should.eq(a.method(:first))
a.method(:size).should.eq([1, 2].method(:size))
true.method(:to_s).should.eq(false.method(:to_s))
a.method(:size).should.eq(a.method(:size).unbind)
a.method(:size).should.eq("size")
