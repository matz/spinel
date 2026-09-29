# A macro called with a receiver writes the state where the expansion does
# not follow: what was known before is not any more (refused, not expanded
# with the stale value).
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
  kind :box
  self.kind :other
  constant :SIZE
end
p Calc::SIZE
