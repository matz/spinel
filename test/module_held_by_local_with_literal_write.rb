# A constant assigned a local that holds a `Module.new` or `Class.new` module
# is followed to it although the program also writes the local a literal
# that is no class (a branch that never runs): where that write ran, the
# include would be CRuby's TypeError, not another module's methods, so the
# literal names no other class. Before, the write made the constant name
# nothing and the include was dropped.
helpers = Module.new do
  def twice(x) = x * 2
end
helpers = 7 if ARGV.size > 99
Helpers = helpers
class Calc
  include Helpers
end
p Calc.new.twice(21)
k = Class.new do
  def base = "base"
end
k = "k" if ARGV.size > 99
k = nil if ARGV.size > 98
K = k
class K
  def more = "more"
end
p K.new.base, K.new.more
