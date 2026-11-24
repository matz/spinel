# A class-body macro defines a name a `def ... unless method_defined?` guard
# asks of later in the class: the guard sees the macro's method, so that def
# is skipped, while a guard before the macro's call finds no method.
# CRuby prints 1 and 3. The default load model does not place a
# define_method a class method's body makes under a computed name, so the
# program is refused until it does.
# spinel: reject-unsupported: Module#define_method with a non-literal name is not supported
class Base
  def self.flag(n) = define_method(n) { 1 }
end

class Foo < Base
  flag :a
  def a = 2 unless method_defined?(:a)
  def b = 3 unless method_defined?(:b)
  flag :b
end

p Foo.new.a, Foo.new.b
