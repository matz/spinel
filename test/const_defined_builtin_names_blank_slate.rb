# A class below BasicObject reaches no Object, so it sees no builtin's
# constant: const_defined? answers false for it.
class Bare < BasicObject; end
class Bare2 < Bare; end
p Bare.const_defined?(:Thread)
p Bare2.const_defined?(:Struct)
b = [Bare, Bare2][1]
p b.const_defined?(:Thread)
p Bare2.const_defined?(["Mutex"].join)
