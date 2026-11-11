# A class value keeps CRuby's singleton methods ahead of a program's Object
# method of the same name: a builtin module or class with no class id
# (Process, GC, Random), a reopened one, and a nested class of the
# program's own that takes a builtin's name, which has none.
class Object
  def pid(*a) = "obj"
  def clock_gettime(*a) = "obj"
  def count = "obj"
  def rand(*a) = "obj"
  def exist?(x) = "obj"
  def basename(x) = "obj"
end
p Process.pid.is_a?(Integer)
p Process.clock_gettime(Process::CLOCK_MONOTONIC).is_a?(Float)
p GC.count.is_a?(Integer)
p Random.rand(1).is_a?(Integer)
module Process; end
p Process.pid.is_a?(Integer)
class Foo
  class Dir; def hi = 1; end
  class File; end
end
p Foo::Dir.new.hi
p Foo::Dir.exist?("/")
p Foo::File.basename("/a/b.rb")
p Dir.exist?("/")
p File.basename("/a/b.rb")
