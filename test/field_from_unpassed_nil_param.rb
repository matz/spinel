# A field written from a `= nil` parameter no call supplies (`@prev = prev`)
# takes the nil it always is, so the field is typed by its other writes:
# Node's @prev and @next hold a Node or nil. Every other class here has a
# call that may supply the parameter (a String, through a send,
# alias_method, a super, a bind_call, a super in self.new; an Integer
# through the runtime's coerce), or a method that writes the parameter;
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

class BySelfNew
  attr_accessor :prev
  def self.new(k) = super(k, "self.new")
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
end
p BySelfNew.new(1).prev

# the runtime calls coerce (`2 + v`) with no call of the program's
class ByCoerce
  attr_accessor :last
  def initialize = (@last = nil)
  def coerce(other = nil)
    @last = other
    [ByCoerce.new, self]
  end
  def +(o) = self
end
v = ByCoerce.new
w = ByCoerce.new
w.last = v
2 + v
p v.last

# an instance_exec block writes the receiver's field
class ExecTarget
  attr_accessor :flag
  def initialize = (@flag = ExecTarget)
end
class ByExec
  def mark(t, value = nil) = t.instance_exec { @flag = value }
end
t = ExecTarget.new
ByExec.new.mark(t)
p t.flag

# a subclass's self.new reaches the initialize it inherits
class BaseInit
  attr_accessor :prev
  def initialize(k, prev = nil) = (@k = k; @prev = prev)
end
class ChildNew < BaseInit
  def self.new(k) = super(k, "child.new")
end
p ChildNew.new(1).prev

# an instance_exec block on a receiver of two classes writes each one's field
class ExecA
  attr_accessor :flag
  def initialize = (@flag = 3)
end
class ExecB
  attr_accessor :flag
  def initialize = (@flag = 4)
end
class ByPolyExec
  def mark(t, value = nil) = t.instance_exec { @flag = value }
end
targets = [ExecA.new, ExecB.new]
ByPolyExec.new.mark(targets[ARGV.size])
p targets[0].flag
ByPolyExec.new.mark(targets[ARGV.size + 1])
p targets[1].flag

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
