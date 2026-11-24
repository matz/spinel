# A send whose target is a macro writes the state, and a later macro builds a
# name from it for a public_send. Neither is expanded; the program's sends
# of computed names pass nothing, so they make no method the load record
# cannot name, and the run-time public_send on the class dispatches over the
# class's methods (it was refused while such a send made every name a
# possible definition): `calc_box_size`, 2.
module Consts
  def kind(k) = (@kind = k)
  def setup(t) = public_send(:kind, t)
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  setup :box
  constant :SIZE
end
p Calc::SIZE
