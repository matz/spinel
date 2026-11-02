# The forms that add a method fire method_added (singleton_method_added for
# a singleton's) too: attr_*, define_method, alias and alias_method, a def
# given to private, module_function, and a def under a condition. A
# visibility word given a method the class holds fires nothing, nor does a
# hook that is not defined.
class Forms
  def self.method_added(n) = puts("added #{n}")
  attr_accessor :v
  define_method(:d) { 1 }
  def a; end
  alias_method :b, :a
  alias c a
  def e; end
  private :e
  private def f; end
  public
  def g; end
end

module Fn
  def self.method_added(n) = puts("Fn added #{n}")
  def self.singleton_method_added(n) = puts("Fn sadded #{n}")
  module_function
  def f = 1
  def self.one = 2
end
puts Fn.f

class Sing
  def self.singleton_method_added(n) = puts("Sing sadded #{n}")
  def self.a; end
  def b; end
end

class Branches
  def self.method_added(n) = puts("Branches #{n}")
  if ARGV.empty?
    def yes; end
  else
    def no; end
  end
  def after; end
end

class Later
  def self.install
    def self.method_added(n) = puts("Later #{n}")
  end
  def a; end
  install
  def b; end
end
