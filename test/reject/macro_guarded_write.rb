# A class-body write of a macro's state under a guard is decided at run time:
# the macro reading it afterwards is left as written (a run-time public_send,
# refused), not expanded with the state from before the guard.
module Consts
  def constant(c)
    const_set(c, public_send(["calc", @kind, c.to_s.downcase].compact.join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_size = 1
  def self.calc_box_size = 2
  @kind = :box if ARGV.empty?
  constant :SIZE
end
p Calc::SIZE
