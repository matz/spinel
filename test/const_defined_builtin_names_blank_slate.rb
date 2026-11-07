# A class below BasicObject reaches no Object, so it sees no builtin's
# constant: const_defined? answers false for it.
class Bare < BasicObject; end
class Bare2 < Bare; end
p Bare.const_defined?(:Thread)
p Bare2.const_defined?(:Struct)
b = [Bare, Bare2][1]
p b.const_defined?(:Thread)
p Bare2.const_defined?(["Mutex"].join)
# BasicObject itself holds only its own name
p BasicObject.const_defined?(:Struct)
p BasicObject.const_defined?(:BasicObject)
p ::BasicObject.const_defined?(["Thr", "ead"].join)
def cd_pass(x) = x
p cd_pass(BasicObject).const_defined?(:Thread)
p cd_pass(Integer).const_defined?(:Thread)
