# A field written from a `= nil` parameter no call supplies (`@prev = prev`)
# takes the nil it always is, so the field is typed by its other writes:
# Node's @prev and @next hold a Node or nil. Every other class here has a
# call that may supply the parameter (a String, through a send,
# alias_method, a super, a bind_call), or a method that writes the parameter;
# its field stays boxed and holds the String.
class Node
  attr_accessor :key, :prev, :next
  def initialize(key, prev = nil, next_node = nil) = (@key = key; @prev = prev; @next = next_node)
end
a = Node.new(1)
b = Node.new(2)
b.prev = a
a.next = b
p b.prev.key, a.next.key, a.prev, b.next

class BySend
  attr_reader :prev
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
end
p BySend.new(1).prev, BySend.send(:new, 1, "send").prev

class ByAliasMethod
  attr_reader :prev
  def link(k, prev = nil) = (@k = k; @prev = prev; self)
  alias_method :join, :link
end
p ByAliasMethod.new.link(1).prev, ByAliasMethod.new.join(1, "alias_method").prev

class BySuper
  attr_reader :prev
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
end
class BySuperKid < BySuper
  def initialize(k) = super(k, "super")
end
p BySuper.new(1).prev, BySuperKid.new(1).prev

class ByZsuper
  attr_reader :prev
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
end
class ByZsuperKid < ByZsuper
  def initialize(k, prev = "zsuper") = super
end
p ByZsuper.new(1).prev, ByZsuperKid.new(1).prev

class ByBind
  attr_reader :prev
  def setup(k, prev = nil) = (@k = k; @prev = prev; self)
end
p ByBind.new.setup(1).prev, ByBind.instance_method(:setup).bind_call(ByBind.new, 1, "bind_call").prev

class Rewritten
  attr_reader :prev
  def initialize(k, prev = nil)
    prev ||= "or-write"
    @prev = prev
  end
end
p Rewritten.new(1).prev

# a super in a method of another name does not reach initialize
class Plain
  attr_accessor :prev
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
  def to_s = "Plain"
end
class PlainKid < Plain
  def to_s = super + "Kid"
end
q = Plain.new(1)
r = Plain.new(2)
r.prev = q
p r.prev.equal?(q), q.prev, PlainKid.new(3).to_s
