# __LINE__ in a macro is the macro source's line: not computed at compile time
# (a run-time public_send, refused), not taken as 0.
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
