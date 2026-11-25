# A program's hash, eql? or <=> that asks block_given? (or yields) is called
# by the runtime's hash-key and sort hooks with no block: they call the
# method's proc-form clone, the function a yielding method has.
class Hk
  def initialize(a) = @a = a
  def a = @a
  def hash
    block_given? ? 0 : @a
  end
end
o = Hk.new(1)
h = {o => 1}
p h[o], h[Hk.new(1)]

# the same with Object's hash behind it, through super
class Hs
  def initialize(a) = @a = a
  def hash
    block_given?
    super
  end
end
q = Hs.new(1)
h = {q => 2}
p h[q], h[Hs.new(1)]

class Ek
  def initialize(a) = @a = a
  def a = @a
  def hash = @a.hash
  def eql?(o)
    return :blk if block_given?
    a == o.a
  end
end
h = {Ek.new(1) => 3}
p h[Ek.new(1)], h[Ek.new(2)]

class Ck
  include Comparable
  def initialize(a) = @a = a
  def a = @a
  def <=>(o)
    yield if block_given?
    a <=> o.a
  end
end
p [Ck.new(3), Ck.new(1), Ck.new(2)].sort.map(&:a)
p [Ck.new(3), Ck.new(1)].max.a, Ck.new(1) < Ck.new(2)
