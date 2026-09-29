# A macro body whose residual call writes the state in a block: what the
# block writes is not followed, so the macro reading it is not expanded (a
# run-time public_send, refused).
module Consts
  def constant(c)
    tap { @kind = :box }
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  constant :SIZE
end
p Calc::SIZE
