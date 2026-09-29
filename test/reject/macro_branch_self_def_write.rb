# A class method the body defines, called inside a branch, may write the
# macro's state: what it writes is unknown (refused, not expanded with the
# value from before it).
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  def self.set_kind = (@kind = :box)
  if ARGV.empty?
    set_kind
  end
  constant :SIZE
end
p Calc::SIZE
