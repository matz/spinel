# A class method an expansion would define, whose body writes the state, is
# called later: the macro defining it is left as written (and refused at run
# time), so the state is not read as unchanged.
module Consts
  def setter(n) = define_singleton_method(n, -> { @kind = :box })
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
