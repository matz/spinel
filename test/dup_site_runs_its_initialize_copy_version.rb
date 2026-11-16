# A dup or clone runs the initialize_copy the program has defined where the
# call runs, not the last one the program defines: two versions with a dup
# between them, one a parent defines after the first dup of a child, a
# Struct's and an Array subclass's, a version that adds a defaulted
# parameter, and a clone of a frozen object (its hook runs on an unfrozen
# copy). Controls that must not move: a hook that supers into a reopened
# parent, an ivar only the later initialize sets, and a clone that keeps
# its singleton.

# a version that reads the source's Integer ivar after one that does not
class Ver
  attr_accessor :n, :tag
  def initialize(n) = @n = n
  def initialize_copy(o) = @tag = :d1
end
ver = Ver.new(4)
p ver.dup.tag
class Ver
  def initialize_copy(o) = @tag = o.n * 2
end
p ver.dup.tag

# clone of a frozen object: the hook sees an unfrozen copy
class Frz
  attr_reader :t
  def initialize_copy(o) = @t = :c1
end
frz = Frz.new.freeze
c = frz.clone
p c.t, c.frozen?
class Frz
  def initialize_copy(o) = @t = [:c2, frozen?]
end
c = frz.clone
p c.t, c.frozen?
p frz.clone(freeze: false).t

# a Struct's hook, from its block and then reopened
S = Struct.new(:a, :b) do
  def initialize_copy(o)
    super
    self.b = :copied1
  end
end
s = S.new(1, 2)
p s.dup.to_a
class S
  def initialize_copy(o)
    super
    self.b = o.a + 100
  end
end
p s.dup.to_a, s.to_a

# a parent's hook defined after the first dup of a child
class Par
  attr_accessor :t
end
class Chi < Par; end
chi = Chi.new
chi.t = :orig
p chi.dup.t
class Par
  def initialize_copy(o) = @t = [o.t, :hooked]
end
p chi.dup.t, chi.clone.t

# an Array subclass
class Arr < Array
  attr_reader :t
  def initialize_copy(o)
    super
    @t = 1
  end
end
arr = Arr.new([1, 2])
d = arr.dup
p d.to_a, d.t, d.class
class Arr
  def initialize_copy(o)
    super
    @t = o.size * 10
  end
end
d = arr.dup
p d.to_a, d.t

# the parameters after the original take their defaults
class Dflt
  attr_reader :t
  def initialize_copy(o) = @t = :one
end
dflt = Dflt.new
p dflt.dup.t
class Dflt
  def initialize_copy(o, k = :dflt) = @t = [:two, k]
end
p dflt.dup.t

# controls: super into a reopened parent
class Base
  attr_reader :log
  def initialize_copy(o) = (@log = [:p1])
end
class Kid < Base
  def initialize_copy(o)
    super
    @log << :c
  end
end
kid = Kid.new
p kid.dup.log
class Base
  def initialize_copy(o) = (@log = [:p2])
end
p kid.dup.log

# an ivar only the later initialize sets, read in the hook
class Iv
  attr_reader :b, :seen
  def initialize = @a = 1
  def initialize_copy(o) = @seen = o.b.nil? ? :was_nil : o.b
end
iv = Iv.new
p iv.dup.seen
class Iv
  def initialize = (@a = 1; @b = 2)
end
p Iv.new.dup.seen

# a clone keeps its singleton, a dup drops it
class Sing
  attr_reader :v
  def initialize_copy(o) = @v = :v1
end
sg = Sing.new
def sg.hi = :hi
p sg.clone.hi, sg.dup.respond_to?(:hi)
class Sing
  def initialize_copy(o) = @v = :v2
end
p sg.clone.v, sg.dup.v
