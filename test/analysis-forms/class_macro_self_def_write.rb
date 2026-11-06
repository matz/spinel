# The text pass refuses this call (test/reject/macro_self_def_write.rb); the
# class-body macro form runs the macro at the call's point under the load
# graph, as CRuby runs it.
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
