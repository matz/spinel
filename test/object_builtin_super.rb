# A `super` from a program class into one of Object's public methods (Kernel's
# or BasicObject's) when no method of the program answers it: the builtin's
# own method on self.

class Plain
  def initialize; @a = 1; @b = :two; end
  def hash = super
  def object_id = super
  def tap = super
  def then = super
  def methods = super
  def dup = super
  def clone = super
  def instance_variables = super
  def instance_variable_get(n) = super
  def <=>(x) = super
  def !=(x) = super
  def eql?(x) = super
  def is_a?(k) = super
  def send(*a, &b) = super
end

o = Plain.new
h = o.hash
p h.is_a?(Integer), h == o.hash
i = o.object_id
p i.is_a?(Integer), i == o.object_id
p o.tap { |y| $t = y }.equal?(o), $t.equal?(o)
p o.then { |y| y.equal?(o) ? 5 : 6 }
p o.methods.include?(:hash), o.methods.include?(:zzq)
d = o.dup
p d.class, d.equal?(o), d.instance_variable_get(:@b)
p o.clone.class
p o.instance_variables
p o.instance_variable_get(:@a)
p (o <=> o), (o <=> 1), o.<=>([o].first)
p (o != o), (o != 1)
p o.eql?(o), o.eql?(1)
p o.is_a?(Plain), o.is_a?(String)
p o.send(:class), o.send(:then) { 7 }

# (a class with no subclass: === is reached as the builtin's)
class Leaf
  def ===(x) = super
end
lf = Leaf.new
p (lf === lf), (lf === 1)

# a subclass, a module in between, three levels
class Sub < Plain; end
s = Sub.new
p s.hash == s.hash, s.dup.class, (s <=> s), s.tap { }.equal?(s)

module Mid
  def hash = super
  def tap = super
end
class WithMid
  include Mid
end
w = WithMid.new
p w.hash.is_a?(Integer), w.tap { }.equal?(w)

class L1; def hash = super; end
class L2 < L1; def hash = super; end
class L3 < L2; def hash = super; end
x = L3.new
p x.hash == x.hash

# the forms of the call: super(), the value used, a branch, method(:x).call,
# a boxed receiver, two calls in one statement
class Forms
  def hash; super() ^ 1; end
  def dup
    r = super
    r
  end
  def object_id
    if ARGV.size > 9 then 0 else super end
  end
end
f = Forms.new
p f.hash.is_a?(Integer), f.dup.class, f.object_id == f.object_id
p f.method(:hash).call == f.hash
b = [Forms.new, 1].first
p b.hash == b.hash, b.dup.class
p [o.hash == o.hash, o.dup.class]

# a program object_id and != are called as they are defined
class OwnId
  def object_id = :mine
  def !=(x) = block_given? ? :blk : :plain
end
p OwnId.new.object_id, (OwnId.new != 1)

# a `(...)` super hands on the arguments, the block and the keywords
class Fwd
  def object_id(...) = super(...)
  def tap(...) = super(...)
  def initialize = @v = 3
end
fw = Fwd.new
p fw.object_id.is_a?(Integer), fw.object_id == fw.object_id
p fw.tap { |y| $u = y }.equal?(fw), $u.equal?(fw)
