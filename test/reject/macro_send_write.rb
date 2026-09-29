# A send whose target is a macro writes the state: a later macro reading it
# is left as written (a run-time public_send, refused).
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
