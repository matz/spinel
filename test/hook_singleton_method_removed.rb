# remove_method in the body of a `class << X` calls singleton_method_removed
# on X, with the hook the class has at that point: after the statement, so a
# hook the statement removes is not called, and one defined between two
# removals sees only the second. (Only the singleton side: an instance
# method's remove_method is not supported.)
class Plain
  def self.singleton_method_removed(n) = puts("#{self} removed #{n.inspect}")
  def self.x = 1
  class << self
    remove_method :x
  end
end

# the explicit form at the top level, and several names and a String
class Several
  def self.singleton_method_removed(n) = puts("Several removed #{n.inspect}")
  def self.x; end
  def self.y; end
  def self.z; end
end
class << Several
  remove_method :y, :x
  remove_method "z"
end

# the hook defined between two removals, and one from an extended module
class Between
  def self.x; end
  def self.y; end
  class << self
    remove_method :x
    def singleton_method_removed(n) = puts("Between removed #{n}")
    remove_method :y
  end
end
module H
  def singleton_method_removed(n) = puts("H removed #{n}")
end
class Mixed
  def self.x; end
  def self.y; end
  class << self
    remove_method :x
  end
  extend H
  class << self
    remove_method :y
  end
end

# a hook inherited from the superclass, whose super is the core's no-op
class HookBase
  def self.singleton_method_removed(n)
    r = super
    puts "#{self} removed #{n} #{r.inspect}"
  end
end
class HookSub < HookBase
  def self.x; end
  class << self
    remove_method :x
  end
end

# a module's own singleton, and the body of a class builder's block
module Mod
  def self.singleton_method_removed(n) = puts("Mod removed #{n}")
  def self.x; end
  class << self
    remove_method :x
  end
end
Built = Class.new do
  def self.singleton_method_removed(n) = puts("Built removed #{n}")
  def self.x; end
  class << self
    remove_method :x
  end
end

# removing the hooks themselves: method_removed is a singleton method here
# too, and singleton_method_removed is gone when its own removal is announced
class Hooks
  def self.singleton_method_removed(n) = puts("Hooks removed #{n}")
  def self.method_removed(n) = puts("Hooks instance removed #{n}")
  def self.x; end
  def self.y; end
  class << self
    remove_method :method_removed
    remove_method :x
    remove_method :singleton_method_removed
  end
  class << self
    remove_method :y
  end
end

# added, removed and added again, then undefined, in order
class Order
  def self.singleton_method_added(n) = puts("Order added #{n}")
  def self.singleton_method_removed(n) = puts("Order removed #{n}")
  def self.x; end
  class << self
    remove_method :x
    def x; end
    undef_method :x
  end
end

# an alias keeps the method, a private_class_method one is removed too
class Aliased
  def self.singleton_method_removed(n) = puts("Aliased removed #{n}")
  def self.x = "x"
  def self.p; end
  private_class_method :p
  class << self
    alias y x
    remove_method :x
    remove_method :p
  end
end
puts Aliased.y

# a removed override leaves the superclass's method to answer
class Over
  def self.y = "Over.y"
end
class Under < Over
  def self.y = "Under.y"
  def self.call_y = y
  class << self
    remove_method :y
  end
end
puts Under.y
puts Under.call_y

# a top-level def singleton_method_removed is BasicObject's name, which a
# class without a hook of its own reaches
def singleton_method_removed(n) = puts("top removed #{n}")
class Top
  def self.x; end
  class << self
    remove_method :x
  end
end
puts "done"
