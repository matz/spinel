# A proc read through a reader, or out of an instance variable, reaches only
# the procs written there: an appending lambda elsewhere in the program does
# not make the call hand the instance variable's String over as a copy.
module Views
  def self.default_builder = ->(name) { "<#{name}>" }
  @builder = default_builder
  def self.builder = @builder
end

class Item
  def initialize(name) = @name = name
  def render = Views.builder.call(@name)
end

shout = ->(s) { s << "!" }
puts Item.new("card").render

# the proc in an instance variable of the object itself
class Badge
  def initialize(name)
    @name = name
    @fmt = ->(n) { "[#{n}]" }
  end
  def render = @fmt.call(@name)
end
puts Badge.new("badge").render

# a reader that chooses between procs, one of them by an explicit return
module Pick
  def self.loud = ->(n) { n.upcase }
  def self.quiet = ->(n) { n.downcase }
  def self.for(flag)
    return loud if flag
    quiet
  end
end

class Label
  def initialize(name) = @name = name
  def render(flag) = Pick.for(flag).call(@name)
  def name = @name
end
l = Label.new("Label")
puts l.render(true)
puts l.render(false)
puts l.name

# a reader of a reader
module Outer
  def self.inner = Views.builder
end
class Tag
  def initialize(name) = @name = name
  def render = Outer.inner.call(@name)
end
puts Tag.new("tag").render
puts shout.call(+"done")
