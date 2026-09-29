# A class method the body defines and calls may write the macro's state: what
# it writes is unknown (refused, not expanded with the value before it).
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  def self.calc_other_size = 3
  def self.set = @kind = :box
  set
  constant :SIZE
end
p Calc::SIZE
