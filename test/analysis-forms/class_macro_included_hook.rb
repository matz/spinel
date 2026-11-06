# The text pass refuses this call (test/reject/macro_included_hook.rb); the
# class-body macro form runs the macro at the call's point under the load
# graph, as CRuby runs it.
module Setup
  def self.included(base) = base.instance_variable_set(:@kind, :box)
end
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  include Setup
  constant :SIZE
end
p Calc::SIZE
