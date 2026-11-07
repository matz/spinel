# const_defined? looks past a class's own chain to Object's ancestors: the
# constants of Kernel and of a module Object includes, which every class and
# module sees, and which a class below BasicObject does not.
module Kernel; KC = 1; end
module Mx; MC = 2; end
class Object; include Mx; end
module Comparable; CC = 3; end
class Z; end
class Y; include Comparable; end
class Bare < BasicObject; end

p Z.const_defined?(:KC), Z.const_defined?(:MC)
p Z.const_defined?(:CC), Y.const_defined?(:CC)
p Object.const_defined?(:KC), Object.const_defined?(:MC), Object.const_defined?(:CC)
p Comparable.const_defined?(:KC), Comparable.const_defined?(:MC)
p Bare.const_defined?(:KC), Bare.const_defined?(:MC)
p Z.const_defined?(:KC, false), Object.const_defined?(:KC, false), Kernel.const_defined?(:KC)
p Z.const_get(:KC), Z.const_get(:MC)
