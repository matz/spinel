# Refused by the text pass (test/reject/macro_source_line.rb); the macro form
# runs it under the load graph. Two comment lines: __LINE__ in the macro is 4.
module Consts
  def constant(c) = const_set(c, public_send("calc_#{__LINE__}"))
end
class Calc
  extend Consts
  def self.calc_0 = 1
  def self.calc_4 = 2
  constant :X
end
p Calc::X
