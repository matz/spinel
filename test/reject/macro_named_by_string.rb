# A macro whose every call was expanded is still kept when a string names it
# (`public_send("constant", ...)`): that call reaches it at run time (and the
# macro's own run-time public_send is refused), not a NoMethodError.
module Consts
  def constant(c) = const_set(c, public_send("calc_#{c.to_s.downcase}"))
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_y = 2
  constant :SIZE
end
Calc.public_send("constant", :Y)
p Calc::SIZE, Calc::Y
