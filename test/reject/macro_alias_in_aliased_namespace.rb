# `module M` where M = N reopens N: the `Key = Calc` inside binds N::Key, so
# the class statement N::Key reopens Calc and its macro calls write Calc's state.
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
module N; end
M = N
module M
  Key = Calc
end
module N
  class Key
    kind :b
  end
end
class Calc
  constant :SIZE
end
p Calc::SIZE
