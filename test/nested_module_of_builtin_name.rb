# A module of the program nested under another (Foo::Process) is not the
# builtin of its leaf's name: a top-level Process names the builtin, and its
# singleton methods answer ahead of a program's Object method of the name.
class Object
  def pid(*a) = "obj"
  def count(*a) = "obj"
end
module Foo
  module Process
    def self.hello = "foo process"
  end
  module GC; end
  def self.inside = Process.hello
end
p Process.pid.class
p GC.count.class
p Foo::Process.hello
p Foo.inside
p Foo::Process.pid
p Foo::GC.count
