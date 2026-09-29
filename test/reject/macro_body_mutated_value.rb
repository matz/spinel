# The class body changing a value the state holds in place (`@parts << x`):
# a macro reading the state afterwards is left as written (a run-time
# public_send, refused), not expanded with the old value.
module Consts
  def constant(c)
    const_set(c, public_send((@parts + [c.to_s.downcase]).join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_box_size = 1
  def self.calc_box_x_size = 2
  @parts = ["calc", "box"]
  @parts << "x"
  constant :SIZE
end
p Calc::SIZE
