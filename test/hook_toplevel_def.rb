# Class's and Module's methods stand before Object's, a top-level def among
# them: a def of a hook's name at the top level is no hook, and a class made
# (of a known class, a Struct class or a Data class) fires nothing, nor does
# a send of the name to a class. Module has no `inherited`: a send of it to a
# module reaches the top-level def. `singleton_method_added` is BasicObject's,
# not Class's: its methods stand after Object's, so a top-level def of that
# name is the one a send reaches.
def inherited(k)
  puts "top inh #{k}"
end

class Base; end
class Sub < Base; end
class Sub2 < Sub; end
Pair = Struct.new(:a)
class PairSub < Pair; end
D = Data.define(:x)
class DSub < D; end
Base.__send__(:inherited, Sub)
Base.send(:inherited, Sub)
module Mod; end
Mod.send(:inherited, Base)
def singleton_method_added(n)
  puts "top sma #{n}"
end
Base.send(:singleton_method_added, :q)
Base.__send__(:singleton_method_added, :r)
puts "done"
