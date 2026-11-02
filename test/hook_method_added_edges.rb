# A visibility word fires method_added only for a method the class does not
# hold itself and whose visibility it changes. A `class << X` body calls
# singleton_method_added on the class, written with a name or a path.
class Base
  def m1; end
  def m2; end
end
class Sub < Base
  def self.method_added(n)
    puts "ma #{n}"
  end
  private :m1
  public :m2
  def own; end
  private :own
end

module M; end
class M::C
  def self.singleton_method_added(n)
    puts "sma #{n}"
  end
  class << self
    def x; end
  end
end

module NS
  class K
    def self.singleton_method_added(n)
      puts "nsma #{n}" unless n == :singleton_method_added
    end
    class << self
      def a; end
    end
  end
end
class NS::K
  class << self
    def b; end
  end
end
class Parent
  def inh; end
end
class Child < Parent
  def self.method_added(n) = puts("Child added #{n}")
  def own; end
  private :own
  private :inh
end

class Sing
  def self.singleton_method_added(n) = puts("Sing sadded #{n}")
  def self.a; end
  def b; end
  class << self
    def c; end
  end
end
Built = Class.new do
  def self.singleton_method_added(n) = puts("Built sadded #{n}")
  class << self
    def x; end
  end
end
module NSB
  Nested = Class.new do
    def self.method_added(n) = puts("Nested ma #{n}")
    def a; end
  end
  Mixin = Module.new do
    def self.singleton_method_added(n) = puts("Mixin sadded #{n}")
    def self.y; end
  end
end
NSB::Pathed = Class.new(Object) do
  def self.method_added(n) = puts("Pathed ma #{n}")
  def b; end
end
puts "done"
