# The text pass refuses this call (test/reject/macro_residual_value.rb); the
# class-body macro form runs the macro at the call's point under the load
# graph, as CRuby runs it.
module Consts
  def prefix = lib_prefix
  def constant(c)
    const_set(c, public_send(["calc", prefix, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.lib_prefix = "box"
  def self.calc_size = 1
  def self.calc_box_size = 2
  constant :SIZE
end
p Calc::SIZE
