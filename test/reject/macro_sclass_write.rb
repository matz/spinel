# A class method defined in `class << self` and called from the body may
# write the macro's state: a later macro reading it is left as written (a
# run-time public_send, refused).
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  class << self
    def set_kind = (@kind = :box)
  end
  def self.calc_size = 1
  def self.calc_box_size = 2
  set_kind
  constant :SIZE
end
p Calc::SIZE
