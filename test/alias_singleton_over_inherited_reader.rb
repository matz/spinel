# A class-side alias in a subclass under the name of a `class << self`
# reader its parent declares is the subclass's own class method, a copy of
# the method the alias names: a call of the name on the subclass runs it,
# not the inherited reader's slot. The parent keeps its reader, and an
# alias of the reader itself still reads the subclass's own slot.
class A
  class << self
    attr_accessor :foo
  end
  @foo = 1
  def self.x = 42
  def self.y=(v)
    @y = v * 2
  end
  def self.yv = @y
end
class B < A
  @foo = 2
  class << self
    alias bar foo
    alias foo x
    def via = foo
  end
end
class C < A
  class << self
    alias foo= y=
  end
end
p B.foo, B.via, B.bar, A.foo
C.foo = 5
p C.yv, C.foo
