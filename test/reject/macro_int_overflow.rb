# An Integer past a C long is a Bignum in CRuby: a name built from one is
# not computed at compile time (refused, not expanded with a wrapped value).
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
  def self.calc_0_size = 4
  kind 4294967296 * 4294967296
  constant :SIZE
end
p Calc::SIZE
