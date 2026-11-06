# A macro call in an arm of a class body's `if` (here a modifier) runs as
# one of the body's statements does.
class Base
  def self.mk(n) = define_method(n) { 1 }
end
class Foo < Base
  mk :a if ARGV.empty?
end
p Foo.new.a
