# The text pass refuses this call (test/reject/macro_own_class_method.rb);
# the class-body macro form runs the macro at the call's point under the
# load graph, as CRuby runs it.
module Consts
  def constant(c) = const_set(c, public_send("calc_#{c.to_s.downcase}"))
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.constant(c) = const_set(c, 2)
  constant :SIZE
end
p Calc::SIZE
