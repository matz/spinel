# A module_eval template defining a class method that writes the state: the
# macro is left as written (its run-time module_eval refused), not expanded
# with the state read as unchanged after the method is called.
module Consts
  def setter(n) = module_eval("def self.#{n} = (@kind = :box)")
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  setter :set_kind
  set_kind
  constant :SIZE
end
p Calc::SIZE
