# An extend made in one branch only: which module's macro a later call
# reaches is decided at run time, so the call is left as written (a run-time
# public_send, refused), not expanded with the branch's module.
module Base
  def constant(c) = const_set(c, public_send("calc_base_#{c.to_s.downcase}"))
end
module Override
  def constant(c) = const_set(c, public_send("calc_over_#{c.to_s.downcase}"))
end
class Calc
  extend Base
  def self.calc_base_x = 1
  def self.calc_over_x = 2
  if ARGV.empty?
    extend Override
  end
  constant :X
end
p Calc::X
