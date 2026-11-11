# An implicit-self call in a class's method dispatches only to the classes
# that inherit that method: a descendant overriding it (without super) is no
# self there, so its arm is not emitted and its parameters do not check the
# call's arguments. Gadget#initialize's `size + 1` took Widget#reset's nil
# and the program was refused (#11613).
class Widget
  def initialize(size = nil)
    @size = size
  end

  def reset
    initialize(nil)
    self
  end

  def setup(a, day)
    @day = day
  end

  def again
    setup(1, nil)
    @day
  end

  def size = @size
end

class Gadget < Widget
  def initialize(size = 1)
    @size = size + 1
  end

  def reset
    @size = 0
    self
  end

  def setup(a, day)
    @day = day + a
  end

  def again = setup(1, 2)
end

class Gizmo < Widget
end

p Widget.new(5).reset.size
p Gadget.new(5).reset.size
p Gizmo.new(5).reset.size
p Widget.new.again, Gadget.new.again, Gizmo.new.again

# A descendant whose override calls super does run the method with its own
# self: it keeps its arm.
class Base
  def initialize(n = 0) = @n = n
  def twice = (initialize(@n * 2); @n)
  def n = @n
end

class Child < Base
  def initialize(n = 0) = @n = n + 100
  def twice = super
end

p Base.new(1).twice, Child.new(1).twice
