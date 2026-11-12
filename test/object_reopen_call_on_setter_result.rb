# An Object method called, in statement position, on the value of a
# setter call answers with its own method: the receiver's setter is not
# the method the call runs.
class Wrap
  def initialize(v)
    @v = v
  end

  def eq(o)
    puts(@v == o)
  end

  def show
    p @v
  end
end

class Object
  def should
    Wrap.new(self)
  end

  def boxed
    [self]
  end
end

class Box
  def label=(v)
    1
  end
end

(Box.new.label = "bar").should.eq("bar")
(Box.new.label = "bar").should.show
(Box.new.label = "bar").boxed
p((Box.new.label = "bar").boxed)

klass = Class.new do
  def foobar=(*)
    1
  end
end
(klass.new.foobar = "bar").should.eq("bar")
(klass.new.foobar = "bar", "baz").should.eq(["bar", "baz"])
