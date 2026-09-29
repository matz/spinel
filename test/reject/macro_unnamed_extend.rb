# An extend the walk cannot name (a module chosen at run time) may change
# which module answers a macro: the later call is left as written (a run-time
# public_send, refused), not expanded with the module before it.
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
  extend(ARGV.empty? ? Override : Base)
  constant :X
end
p Calc::X
