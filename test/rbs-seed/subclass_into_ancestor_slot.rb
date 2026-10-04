# A subclass instance flowing into a slot declared as one of its ancestors.
#
# Each class gets its own C struct, and a subclass replicates its parent's
# fields in order at the same offsets, so the pointer is layout-compatible and
# the conversion is a no-op at run time. C still requires it spelled out: clang
# only warns about the implicit form, while GCC 14 made it an error by default,
# so the same emitted C built on one host and not the other.
#
# An RBS signature naming the base type is the ordinary way to reach this --
# without one the slot infers the concrete class and nothing widens.

class Base
  def initialize(n)
    @n = n
  end

  def n
    @n
  end
end

class Widget < Base
end

class Gadget < Widget
end

class Holder
  def initialize(rec)
    @rec = rec
  end

  def rec
    @rec
  end

  def swap(other)
    @rec = other
    self
  end

  # an explicit return of a subclass through a base-typed slot
  def pick(f)
    return Gadget.new(9) if f
    Base.new(1)
  end

  # the same return out of a begin..rescue, which computes the value
  # before popping the rescue's frame
  def pick_rescued(f)
    begin
      return Gadget.new(8) if f
    rescue
    end
    Base.new(2)
  end
end

class Box
  def initialize
    @slot = nil
  end

  def slot=(v)
    @slot = v
  end

  def slot
    @slot
  end
end

# a bare method returning a subclass where the base is declared
def make
  Widget.new(7)
end

# argument, two levels of descent
h = Holder.new(Widget.new(1))
puts h.rec.n
puts h.swap(Gadget.new(2)).rec.n

# explicit return
puts h.pick(true).n
puts h.pick(false).n
puts h.pick_rescued(true).n
puts h.pick_rescued(false).n

# free-method return
puts make.n

# ivar store through a writer, in value position
b = Box.new
b.slot = Widget.new(5)
puts b.slot.n

# an unrelated class is NOT cast: it would paper over a real mismatch
class Other
  def initialize(n)
    @n = n
  end

  def n
    @n
  end
end

puts Other.new(3).n
