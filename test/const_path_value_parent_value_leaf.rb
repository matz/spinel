# `m::Foo` with m a value, where classes in two scopes share the leaf Foo
# and a module's own Foo is a constant, not a class: the path answers that
# constant (an Integer, a String, a class it holds), as CRuby looks
# constants up by name whatever they hold. A holder it inherits or
# includes counts, and a NameError before the write names the holder.
class Foo; def self.who = "top"; end
class Real; def who = "real"; end
module Ext; Foo = 5; end
module Oth; class Foo; def self.who = "oth"; end; end
module Str; Foo = "str"; end
module Al; Foo = Real; end
class Base; Foo = :base; end
class Kid < Base; end
module Mix; Foo = 7; end
class Inc; include Mix; end
module A; module Deep; Foo = 9; end; end
module Late; Foo = 3 if ARGV.size > 5; end

def look(m) = m::Foo
[Ext, Mix, Inc, A::Deep].each { |m| p look(m) }
w = Str
p w::Foo
w = Al
puts w::Foo.new.who
[Base, Kid].each { |m| p m::Foo }
[Oth, Object].each { |m| puts m::Foo.who }
w = Late
begin
  p w::Foo
rescue NameError => e
  puts e.message
end
