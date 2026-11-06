# The text pass refuses this call (test/reject/macro_const_alias_reopen.rb);
# the class-body macro form runs the macro at the call's point under the
# load graph, as CRuby runs it.
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
Alias = Calc
class Alias
  kind :b
end
class Calc
  constant :SIZE
end
p Calc::SIZE
