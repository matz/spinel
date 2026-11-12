# `class Calc` inside `class Object` reopens the top-level Calc: its macro
# calls write the state the macro reads, which the form answers in order.
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
class Object
  class Calc
    kind :b
  end
end
class Calc
  constant :SIZE
end
p Calc::SIZE
