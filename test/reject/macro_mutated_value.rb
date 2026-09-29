# A macro changing a value the state holds in place (`@parts << x`) is not
# followed: it is not expanded, and a later macro reading the state is left as
# written (a run-time public_send, refused) -- not expanded with the old value.
module Consts
  def suffix(x) = @parts << x
  def constant(c)
    const_set(c, public_send((@parts + [c.to_s.downcase]).join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_box_size = 1
  def self.calc_box_x_size = 2
  @parts = ["calc", "box"]
  suffix "x"
  constant :SIZE
end
p Calc::SIZE
