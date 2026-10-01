# A constant written in a block binds in the block's lexical scope, but its
# scope is not followed: it is matched by name, and Key's body is not tracked.
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_a_size = 1
  def self.calc_b_size = 2
  kind :a
end
[1].each { Key = Calc }
class Key
  kind :b
end
class Calc
  constant :SIZE
end
p Calc::SIZE
