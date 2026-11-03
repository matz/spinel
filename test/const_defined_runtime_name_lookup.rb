# const_defined? with a name known only at run time looks where CRuby
# looks: a module or a class also sees Object's constants (unless the
# second argument is false), and a builtin name such as String counts.
module M
  class Inner
    Z = 3
  end
  W = 1
end
class Blank < BasicObject
  Q = 1
end
X = 5
def name_of(*parts) = parts.join

p Object.const_defined?(name_of("M::Inner"))
p Object.const_defined?(name_of("M::Nope"))
p Object.const_defined?(name_of("String"))
p Object.const_defined?(name_of("Comparable"))
p Object.const_defined?(name_of("X"))
p Object.const_defined?(name_of("Nope"))
p M.const_defined?(name_of("W"))
p M.const_defined?(name_of("X"))
p M.const_defined?(name_of("X"), false)
p M.const_defined?(name_of("String"))
p M.const_defined?(name_of("Inner::Q"))
p M::Inner.const_defined?(name_of("Z"))
p M::Inner.const_defined?(name_of("String"))
p M::Inner.const_defined?(name_of("W"))
p M::Inner.const_defined?(name_of("Nope"))
p Blank.const_defined?(name_of("Q"))
p Blank.const_defined?(name_of("String"))
p Blank.const_defined?(name_of("X"))
