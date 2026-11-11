# `class ::File` / `module ::Process` inside a module's body reopens the
# top-level builtin, so a program's Object method of the name does not win
# over the builtin's singleton methods, and a nested `Foo::Process` does not
# take the name.
class Object; def basename(*a) = "obj"; def pid(*a) = "obj"; end
module M
  class ::File; def self.extra = :x; end
  module ::Process; def self.extra = :y; end
end
p File.extra, Process.extra
p File.basename("/a/b.rb")
p Process.pid.class
K = String
module N
  class ::String; def shout = upcase + "!"; end
end
p K.new("ab").shout
p "q".shout
module Foo; module GC; end; end
p GC.count.class
