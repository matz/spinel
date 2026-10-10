# A lambda or proc in a class method that a subclass inherits reads self as
# the class the method was called on. The method takes that class as a
# parameter, so the proc carries it in rather than naming the defining class.
class Runnable
  def self.tag = "runnable"

  def self.run_suite(names)
    current = nil
    @handler = lambda { "#{self}##{current}" }
    names.each { |n| current = n }
    @handler.call
  end

  def self.make
    lambda { |x| inner = proc { [self, x, tag] }; inner.call }
  end
end

class Sub < Runnable
  def self.tag = "sub"
end

p Runnable.run_suite([:a, :b])
p Sub.run_suite([:c])
p Runnable.make.call(1)
p Sub.make.call(2)
