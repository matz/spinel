# A Method made from an instance variable that holds a class is that
# class's class method, as it is for a constant or a local.

class Box
  def self.pick(value) = 17
  def self.label(name, mark = "!") = "#{name}#{mark}"
  def pick(value) = "instance"
end

class Holder
  def initialize
    @kind = Box
  end

  def loaded = @kind.method(:pick).call(nil)
  def all(values) = values.map(&@kind.method(:pick))
  def picker = @kind.method(:pick)
  def labeler = @kind.method(:label)
end

h = Holder.new
p h.loaded
p h.all([nil, 1, "x"])
m = h.picker
p m.call(5)
p m.arity
p m.owner
p m.name
l = h.labeler
p l.call("a")
p l.call("b", "?")
p l.arity
p [1, 2].map(&h.picker)

# written in the parent's initialize, read in a subclass's method
class Base
  def initialize
    @kind = Box
  end
end

class Child < Base
  def child_loaded = @kind.method(:pick).call(:child)
end
p Child.new.child_loaded

# `||=` keeps the class the constructor wrote
class Lazy
  def initialize
    @kind = Box
  end

  def lazy_loaded
    @kind ||= Box
    @kind.method(:pick).call(nil)
  end
end
p Lazy.new.lazy_loaded

# read before the constructor's write: nil, and CRuby's NameError at the lookup
class Early
  attr_reader :seen

  def initialize
    @seen = begin
      early_picker
    rescue NameError => e
      [e.class, e.message]
    end
    @kind = Box
  end

  def early_picker = @kind.method(:pick)
end
e = Early.new
p e.seen
p e.early_picker.call(3)

# inside a block of the method
class Blocky
  def initialize
    @kind = Box
  end

  def block_loaded = [1, 2].map { |v| @kind.method(:pick).call(v) }
end
p Blocky.new.block_loaded
