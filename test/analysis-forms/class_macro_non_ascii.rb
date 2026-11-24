# The class-body macro form runs the macro
# at the call's point, as CRuby runs it.

module Consts
  def constant(c) = const_set(c, public_send("calc_#{"é".size}"))
end
class Calc
  extend Consts
  def self.calc_1 = 1
  def self.calc_2 = 2
  constant :SIZE
end
p Calc::SIZE
