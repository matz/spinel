# A class's own `def self.include` (or prepend, extend) is what a bare
# `include M` in its body calls: that method runs, and the module is not
# mixed in. A superclass's or an extended module's method counts too, and
# a class among the arguments raises no TypeError there.
module M
  def hi = :m
end

class Plugin
  def self.include(*k) = puts("hook include #{k.inspect}")
  include M
end
p Plugin.new.respond_to?(:hi)

class P2
  def self.extend(k) = puts("hook extend #{k}")
  extend M
end
p P2.respond_to?(:hi)

class Base; end
class P3
  def self.prepend(k) = puts("hook prepend #{k}")
  prepend Base
end
p P3.ancestors.include?(Base)

class P4
  def self.include(*k) = (puts("self form #{k.inspect}"); :done)
  r = self.include(M, Comparable)
  p r
end
p P4.include?(Comparable)

class Parent
  def self.include(*k) = puts("parent hook #{self}")
end
class Child < Parent
  include M
end
p Child.new.respond_to?(:hi)

module Hooky
  def include(*k) = puts("hooky #{k.inspect}")
end
class P5
  extend Hooky
  include M
end
p P5.new.respond_to?(:hi)

module Ns
  def self.extend(*k) = puts("module hook #{k.inspect}")
  extend M
end
p Ns.respond_to?(:hi)

class P6
  class << self
    private def include(*k) = puts("private hook #{k.inspect}")
  end
  include M
end
p P6.new.respond_to?(:hi)

class P7
  def self.include(*k) = puts("from outside #{k.inspect}")
end
P7.include(M)
p P7.new.respond_to?(:hi)

# a def after the include: the builtin include ran first
class P8
  include M
  def self.include(*k) = puts("late")
end
p P8.new.respond_to?(:hi)

# the module's included hook does not fire when the own method answers
module Watched
  def self.included(b) = puts("included in #{b}")
end
class P9
  include Watched
end
class P10 < Parent
  include Watched
end

# a singleton mixin is the singleton's builtin one, not the class's own extend
module Sg
  def sg = :sg
end
class P11
  def self.extend(*k) = puts("own extend")
  singleton_class.include Sg
end
p P11.respond_to?(:sg)

# the definition in effect at the statement answers it
module N
  def ho = :n
end
class P12
  def self.include(*k) = puts("first #{k.inspect}")
  include M
  def self.include(*k) = (puts("second"); super)
end
p P12.new.respond_to?(:hi)
