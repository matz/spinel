# An included hook writing the class's state through its base: a later macro
# reading it is left as written (a run-time public_send, refused).
module Setup
  def self.included(base) = base.instance_variable_set(:@kind, :box)
end
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  include Setup
  constant :SIZE
end
p Calc::SIZE
