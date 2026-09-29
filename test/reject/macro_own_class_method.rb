# A class method of the class's own by a macro's name answers the call, not
# the extended module's macro: the call is left as written (and the macro,
# kept for it, is refused), not expanded with the module's.
module Consts
  def constant(c) = const_set(c, public_send("calc_#{c.to_s.downcase}"))
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.constant(c) = const_set(c, 2)
  constant :SIZE
end
p Calc::SIZE
