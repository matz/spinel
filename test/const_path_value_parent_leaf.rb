# `m::Foo` with m a value names the Foo that m's class or module holds, at
# run time: a local, a block parameter, self in a class method and
# `self.class` in an instance method. Classes in two scopes share the leaf
# Foo, so the path is not the leaf-named class (#10955). A class reaches a
# holder it inherits or includes; Object's own Foo only through Object
# itself; any other value raises CRuby's NameError.
class Foo; def who = "top"; end
module Ext
  class Foo; def who = "ext"; end
  def self.make = self::Foo.new
end
module Oth; class Foo; def who = "oth"; end; end
class Sub; include Ext; end
class Base
  class Foo; def who = "base"; end
  def make = self.class::Foo.new
end
class Kid < Base; end
module Empty; end

m = Ext
puts m::Foo.new.who
x = Oth::Foo.new
[Ext, Oth].each { |mod| p x.is_a?(mod::Foo) }
[Ext, Oth, Object, Sub].each { |mod| puts mod::Foo.new.who }
puts Ext.make.who
puts Kid.new.make.who
case x
when m::Foo then puts "ext arm"
else puts "other arm"
end
[Empty, Kid].each do |mod|
  begin
    mod::Foo
  rescue NameError => e
    puts e.message
  end
end
