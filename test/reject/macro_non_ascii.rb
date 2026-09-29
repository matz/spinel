# String#size / #downcase over non-ASCII text count and map characters in
# CRuby: not computed at compile time from bytes (a run-time public_send,
# refused), not taken as the byte count.
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
