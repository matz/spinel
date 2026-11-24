# spinel: int64 -- 2^64 is an Integer past 2^31; not run on a 32-bit target
# An Integer past a C long is a Bignum in CRuby: a name built from one is
# not computed at compile time with a wrapped value (that would answer
# calc_0_size, 4). The macro is left as written, and its run-time public_send
# builds the name from the Bignum: calc_18446744073709551616_size, 5.
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
  def self.calc_18446744073709551616_size = 5
  kind 4294967296 * 4294967296
  constant :SIZE
end
p Calc::SIZE
