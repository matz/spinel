# A `new` written in a module a class extends, forwarding to super inside an
# ensure (concurrent-ruby's SafeInitialization), runs as a copy for each
# class that extends or inherits it. The copy for a subclass allocates the
# subclass, and its super's value was cast to the pointer type of the class
# the module was first extended onto, where the copy's slot is the
# subclass's: the C did not build. The value is the copy's own class.
module Synchronization
  def self.full_memory_barrier = nil
end
module SafeInitialization
  def new(*args, &block)
    super(*args, &block)
  ensure
    Synchronization.full_memory_barrier
  end
end
class MutexAtomicFixnum
  extend SafeInitialization
  def initialize(v = 0) = @value = v
  def value = @value
  def increment(n = 1) = @value += n
end
class AtomicFixnum < MutexAtomicFixnum
end
class Counter < AtomicFixnum
  def initialize(v = 10) = super(v * 2)
end
a = AtomicFixnum.new(3)
p a.class, a.value, a.increment
m = MutexAtomicFixnum.new
p m.class, m.value
c = Counter.new
p c.class, c.value, c.increment(5)
p AtomicFixnum.new { |x| x }.class
