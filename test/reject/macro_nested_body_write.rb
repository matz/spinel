# A nested class body runs where it stands: a macro it calls on the outer
# class by name writes that class's state, so a later macro reading it is
# left as written (a run-time public_send, refused).
module Consts
  def kind(k)
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_box_size = 1
  def self.calc_other_size = 2
  kind :box
  class Inner
    Calc.kind :other
  end
  constant :SIZE
end
p Calc::SIZE
