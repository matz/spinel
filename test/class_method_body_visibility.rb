# A class method's visibility that a method body's word sets: the calls,
# sends, respond_to?, defined?, a boxed receiver, the class's own list (in a
# method or at the top level) and a Method read it where they run.
class Counter
  def self.make = :made
  def self.tally = :tally
  def self.lock
    private_class_method :make
    nil
  end
  def self.unlock
    public_class_method :make
    nil
  end
  def self.hide_tally
    class << self
      private :tally
    end
    nil
  end
  def self.lock_by_send
    send(:private_class_method, :tally)
    nil
  end
  def self.made_privately
    r = private_class_method def self.fresh = :fresh
    [r, fresh]
  end
end

class Other
  def self.make = :other
end

def report(tag)
  p [tag, (Counter.make rescue $!.class), (Counter.public_send(:make) rescue $!.class), Counter.send(:make)]
  p [tag, Counter.respond_to?(:make), Counter.respond_to?(:make, true), defined?(Counter.make)]
  p [tag, [Counter, Other].map { |k| (k.make rescue $!.class) }]
  p [tag, Counter.singleton_methods(false).include?(:make), Counter.method(:make).call]
end

report(:start)
Counter.lock
report(:locked)
Counter.unlock
report(:unlocked)
Counter.lock
p Counter.singleton_methods.include?(:make)
p [(Counter.tally rescue $!.class), Counter.hide_tally, (Counter.tally rescue $!.class)]
Counter.unlock
p Counter.made_privately
p [(Counter.fresh rescue $!.class), Counter.send(:fresh), Counter.respond_to?(:fresh)]
p [Counter.lock_by_send, (Counter.tally rescue $!.class)]
p Counter.singleton_methods(false).sort

class Shelf
  def self.a = :a
  def self.b = :b
  def self.hide
    private_class_method :a
    class << self
      private :b
    end
    nil
  end
end
class Rack < Shelf
end
Shelf.hide
p [Shelf.singleton_methods.sort, Shelf.singleton_methods(false).sort, Rack.singleton_methods.sort]
