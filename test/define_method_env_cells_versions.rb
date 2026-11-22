# Two definers of one name in different methods, both closing over their
# method's parameter, are two versions of the method, each with an environment
# of its own: the call answers the one that ran last, from the value its
# method had been given, however many times either method has run
class Versions
  def self.one(a) = define_method(:m) { a + 1 }
  def self.two(b) = define_method(:m) { b + 2 }
end
Versions.one(10)
p Versions.new.m
Versions.two(20)
p Versions.new.m
Versions.one(30)
p Versions.new.m


# a module's method made by a module-level definer, read through two includers: the environment is the definer's,
# not a copy's
module Shared
  def self.setup(n)
    define_method(:shared) { n }
    :ok
  end
end
class IncA; include Shared; end
class IncB; include Shared; end
Shared.setup(3)
p IncA.new.shared, IncB.new.shared
Shared.setup(4)
p IncA.new.shared, IncB.new.shared
