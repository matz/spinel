# Module's method_added stands before Object's methods: a top-level def of
# the name is no hook, and adding methods fires nothing. A class's own hook is
# the one that runs, beside a top-level def of the same name.
# singleton_method_added is BasicObject's, which stands after Object's: a
# top-level def of that name is the hook a send reaches.
def method_added(n)
  puts "top #{n}"
end
def singleton_method_added(n)
  puts "top sma #{n}"
end
class Plain
  def bar; end
  attr_reader :q
end

class Hooked
  def self.singleton_method_added(n) = puts("Hooked sma #{n}")
  def self.method_added(n) = puts("Hooked ma #{n}")
  def a; end
  def self.x; end
end

# a class or module a builder makes: the same
Built = Class.new do
  def self.singleton_method_added(n) = puts("Built sma #{n}")
  def self.method_added(n) = puts("Built ma #{n}")
  def b; end
  def self.y; end
end
Sub = Class.new(Hooked) do
  def self.z; end
end
Mixin = Module.new do
  def self.singleton_method_added(n) = puts("Mixin sma #{n}")
  def self.method_added(n) = puts("Mixin ma #{n}")
  def c; end
end

Plain.send(:singleton_method_added, :q)
Plain.__send__(:singleton_method_added, :r)
puts "done"
