# The class-body macro form takes a macro whose run returns before it
# defines anything: that call defines nothing, the next defines `a`.
class Base
  def self.mk(n)
    return if n == :skip
    define_method(n) { 1 }
  end
end
class Foo < Base
  mk :skip
  mk :a
end
p Foo.instance_methods(false)
