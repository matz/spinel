# A module of a builtin's name opened in Object's body (`class Object;
# module Process`) is the builtin itself, as a top-level header is: CRuby's
# Object holds the top-level constants. Its singleton methods and the
# builtin's answer, ahead of a program's Object method of the name, and a
# module of the name inside another class's or module's body stays that
# namespace's own.
class Object
  def count(*a) = "obj"
  module Process
    LIMIT = 7
    def self.hello = :hi
    def self.lim = LIMIT
  end
  module GC
    def self.hello = :gc
  end
end
p Process.hello
p Process.lim
p Process::LIMIT
p GC.hello
p GC.count.class
def gc_hello = GC.hello
p gc_hello
p [1].map { Process.hello }
class ::Object
  module GC
    def self.again = :again
  end
end
p GC.again
