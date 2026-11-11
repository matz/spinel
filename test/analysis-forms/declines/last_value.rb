# The class-body macro form declines: a call that ends an arm of a
# conditional ending the class body, whose value the macro's last
# statement does not give.
class Base
  def self.mk(n)
    if n == :a
      define_method(n) { 1 }
    else
      define_method(:other) { 2 }
    end
  end
end
class Foo < Base
  if true
    mk :a
  end
end
p Foo.new.a
