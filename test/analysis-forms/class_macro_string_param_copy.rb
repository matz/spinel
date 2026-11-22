# The class-body macro form takes a macro that copies a String it was handed
# into a local its block reads: no other local the kept code reads holds that
# String, so the copy written as a new String of its bytes is the object the
# block answers, and a change through the method shows (CRuby answers "a1!").
class Base
  def self.mk(s0)
    s = s0
    define_method(:px) { s }
  end
end
class Foo < Base
  mk "a#{1}"
end
f = Foo.new
f.px << "!"
p f.px
