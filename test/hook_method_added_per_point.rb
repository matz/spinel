# Module#method_added and singleton_method_added fire where CRuby fires
# them: when a method is added, with the hook the class has at that point.
# A hook defined after a def does not fire for it (and fires for none when
# it comes after every def), one removed stops firing, a module's hook
# extended between two defs fires for the later one, a hook under a
# run-time condition fires once its branch has run, a hook defined with
# define_singleton_method fires for the defs after it, and a module's own
# hook fires for the module's defs. Where a class's hook counts, the count
# is the number of defs after it.
class After
  def a; end
  def self.method_added(n) = puts("After #{n}")
  def b; end
end

class AllDefs
  def a; end
  def b; end
  def self.method_added(n) = puts("AllDefs #{n}")
end

class Removed
  def self.method_added(n) = puts("Removed #{n}")
  def a; end
  class << self
    remove_method :method_added
  end
  def b; end
end

module Tracker
  def method_added(n) = puts("Tracker #{n}")
end
class Extended
  def a; end
  extend Tracker
  def b; end
end

class Cond
  def a; end
  if ARGV.empty?
    def self.method_added(n) = puts("Cond #{n}")
  end
  def b; end
end

class Defined
  def a; end
  define_singleton_method(:method_added) { |n| puts "Defined #{n}" }
  def b; end
end

module Mod
  def self.method_added(n) = puts("Mod #{n}")
  def a; end
end
class UsesMod
  include Mod
  def b; end
end

class Counter
  @n = 0
  def self.n = @n
  def a; end
  def self.method_added(_) = @n += 1
  def b; end
  def c; end
end
puts Counter.n

# the hook of a class that is redefined between defs, and one that calls super
class Redef
  def self.method_added(n) = puts("v1 #{n}")
  def a; end
  def self.method_added(n) = puts("v2 #{n}")
  def b; end
end
class Base
  def self.method_added(n) = puts("Base saw #{n}")
end
class Sub < Base
  def self.method_added(n)
    super
    puts("Sub saw #{n}")
  end
  def x; end
end
