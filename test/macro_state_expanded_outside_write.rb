# A macro called on the class from outside its body, between two bodies,
# writes the state: a later body's macro reading it is left as written (a
# run-time public_send, refused).
module Consts
  def kind(k) = (@kind = k)
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
end
Calc.kind :box
class Calc
  constant :SIZE
end
p Calc::SIZE
