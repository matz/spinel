# A module_eval template defining a class method that writes the state, which
# is called outside the class body: a later macro reading the state is left as
# written (a run-time public_send over the class's calc_* methods), not expanded with the state read
# as unchanged.
module Setters
  def setter(n) = module_eval("def self.#{n} = (@kind = :box)")
end
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Setters
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  setter :set_kind
end
Calc.set_kind
class Calc
  constant :SIZE
end
p Calc::SIZE
