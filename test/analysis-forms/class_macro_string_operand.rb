# A block that reads a frozen literal the macro took as an operand: the
# literal of its bytes is the same object, written where the block is.
class Base
  def self.mk(n, d) = define_method(n) { |v = nil| v || d }
end
class Foo < Base
  mk :a, "dflt"
end
p Foo.new.a, Foo.new.a("x")
