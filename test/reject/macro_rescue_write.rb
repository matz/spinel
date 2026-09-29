# An ivar a macro writes in a begin body that has a rescue is decided at run
# time (refused, not expanded with the value before it).
module Consts
  def set_kind(k)
    begin
      @kind = k
    rescue ArgumentError
    end
  end
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  set_kind :box
  constant :SIZE
end
p Calc::SIZE
