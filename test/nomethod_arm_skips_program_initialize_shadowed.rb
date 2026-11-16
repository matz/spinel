# As nomethod_arm_skips_program_initialize, where a module defines a
# NoMethodError and a NameError of its own: the dispatcher's arm names the
# builtins as ::NoMethodError and ::NameError, and still builds them without
# the program's initialize. Run with an argument, so the conditional defs
# never run.
class NoMethodError
  def initialize(m = nil) = super("own: " + m.to_s)
end
class NameError
  def initialize(msg = nil, *rest, **kw) = super("ownN: #{msg}", *rest, **kw)
end
module M
  class NoMethodError < StandardError; end
  class NameError < StandardError; end
end
class Foo
  def run = tag2
end
if ARGV.empty?
  class Foo
    def tag = 1
    def take(r) = r
    def tag2 = 2
  end
end
p((Foo.new.tag rescue [$!.class, $!.message, $!.name]))
p((Foo.new.take(1..2) rescue [$!.class, $!.message, $!.name]))
p((Foo.new.run rescue [$!.class, $!.message, $!.name]))
