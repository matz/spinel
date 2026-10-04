# A constant before `rescue` in a superclass in parentheses is looked up as
# CRuby looks it up. Foo::Base is not the top-level Base: CRuby looks for
# Base in Foo alone, rescues the NameError and takes Other. A bare X in a
# module body finds the module's own X, also when the module is reopened.
class Base
  def hello = "base"
end

class Other
  def hello = "other"
end

class Foo
end

module Mod
end

class W < (Foo::Base rescue Other)
end

class V < (Mod::Base rescue Other)
end

module M
  class X
    def hello = "m::x"
  end

  class Y < (X rescue Base)
  end
end

module M
  class Z < (X rescue Base)
  end
end

class T < (M::X rescue Base)
end

[W, V, M::Y, M::Z, T].each do |k|
  puts "#{k} < #{k.superclass}: #{k.new.hello}"
end
