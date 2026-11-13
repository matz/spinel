class Ctl
  def session = "sess"
end
class Other
  def session = "other"
end
class H
  def initialize(c)
    @c = c
  end
  def who = @c.session
end

# a module body's lambda, called through a reader
module V
  @b = ->(c) { H.new(c) }
  def self.b = @b
end
puts V.b.call(Ctl.new).who

# a module body's lambda, called inside a singleton method
module Views
  @builder = ->(name) { "<#{name}>" }
  def self.render(n) = @builder.call(n)
end
puts Views.render("card")

# a class body instead of a module body
class Page
  @fmt = ->(name) { "[#{name}]" }
  def self.fmt(n) = @fmt.call(n)
end
puts Page.fmt("title")

# two call sites with the same class
module Twice
  @wrap = ->(s) { s.upcase }
  def self.a = @wrap.call("one")
  def self.b = @wrap.call("two")
end
puts Twice.a
puts Twice.b

# a call through the reader and a direct call
module Both
  @tag = ->(s) { "<#{s}>" }
  def self.tag = @tag
  def self.direct(s) = @tag.call(s)
end
puts Both.tag.call("r")
puts Both.direct("d")

# two parameters
module Two
  @join = ->(a, b) { "#{a.upcase}-#{b.session}" }
  def self.join(a, b) = @join.call(a, b)
end
puts Two.join("x", Ctl.new)

# call sites of different types widen the parameter
module Mixed
  @show = ->(v) { v.inspect }
  def self.show(v) = @show.call(v)
end
puts Mixed.show("s")
puts Mixed.show(42)
puts Mixed.show(:sym)
puts Mixed.show(Ctl.new.session)

# a reader called with different objects
module Many
  @mk = ->(c) { H.new(c) }
  def self.mk = @mk
end
puts Many.mk.call(Ctl.new).who
puts Many.mk.call(Other.new).who

# a singleton attr_reader answers the body's ivar
module SgReader
  @b = ->(c) { c.session }
  class << self
    attr_reader :b
  end
end
puts SgReader.b.call(Ctl.new)

# a singleton attr_accessor on a class body
class SgAccessor
  @b = ->(c) { "#{c.session}!" }
  class << self
    attr_accessor :b
  end
end
puts SgAccessor.b.call(Other.new)

# an instance attr_reader answers the ivar initialize writes
class InstReader
  attr_reader :b
  def initialize
    @b = ->(c) { c.session }
  end
end
puts InstReader.new.b.call(Ctl.new)

# a subclass declares the reader of its parent's ivar
class Base
  def initialize
    @fn = ->(c) { c.session.upcase }
  end
end
class Derived < Base
  attr_reader :fn
end
puts Derived.new.fn.call(Other.new)
