# A class or module header in Object's body names what it names at the top
# level, as CRuby's Object holds the top-level constants: `class Object`
# there reopens Object itself, and `class Foo` there is the top-level Foo.
class Object
  class Object
    def zz = 1
    module Process
      def self.hello = :oo
    end
  end
  class Foo
    def hi = :foo
  end
end
class Foo
  def ho = :ho
end
p 5.zz
p Object::Object.equal?(Object)
p Process.hello
p Foo.new.hi
p Foo.new.ho
p Object::Foo.equal?(Foo)

# a `class ::Object` inside a namespace leaves the namespace's own headers after it in the namespace
module M
  class ::Object
    module Zed
      def self.z = 1
    end
  end
  module Util
    def self.m = 2
  end
end
module Util
  def self.t = 3
end
p Zed.z
p M::Util.m
p Util.t
p M::Util.respond_to?(:t)
p Util.respond_to?(:m)
p M::Util.name
class Outer
  class ::Object
    class Helper
      def h = :h
    end
  end
  class Item
    def i = :outer_item
  end
end
class Item
  def i = :top_item
end
p Helper.new.h
p Outer::Item.new.i
p Item.new.i
p Outer::Item.name
