# spinel: needs-rbs-parser
# spinel: share
# Standalone `# @rbs @ivar: T` annotations inside statementless (annotation-only)
# class and module bodies attach to the enclosing class/module node and seed the
# ivar cleanly, including in subclasses, reopenings, modules, and with aliases.

class Base
  def initialize
    @x = nil
    @label = nil
  end

  def x = @x
  def label = @label

  def set_x(v)
    @x = v
  end

  def set_label(v)
    @label = v
  end
end

class Sub < Base
  # @rbs type label_t = String?
  # @rbs @x: Integer?
  # @rbs @label: label_t
end

class Holder
  def initialize
    @count = nil
  end

  def count = @count
  def bump(n) = (@count = n)
end

class Holder
  # @rbs @count: Integer?
end

module TagMixin
  # @rbs @tag: String?
end

class Tagged
  include TagMixin

  def initialize
    @tag = nil
  end

  def tag = @tag
  def tag=(v)
    @tag = v
  end
end

module Ns
  class EmptyChild < Base
    # @rbs @x: Integer?
  end
end

s = Sub.new
p s.x, s.label
s.set_x(11)
s.set_label("ok")
p s.x, s.label

h = Holder.new
p h.count
h.bump(3)
p h.count

t = Tagged.new
p t.tag
t.tag = "v1"
p t.tag

c = Ns::EmptyChild.new
p c.x
c.set_x(99)
p c.x
