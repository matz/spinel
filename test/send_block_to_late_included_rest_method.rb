# A class that takes a module's method after the class is first defined:
# the method a dynamic send reaches on it, with a block and no arguments
class Box
  def initialize(o) = @o = o
  def size = @o.size
end
class Bag < Box; end
module Tryable
  def try(*args, **kwargs, &block)
    if args.empty? && kwargs.empty? && block_given?
      yield self
    elsif respond_to?(args.first)
      public_send(*args, **kwargs, &block)
    end
  end
end
class Object; include Tryable; end
class Box; include Tryable; end
class Call
  def initialize(m) = @method_name = m
  def make_lambda
    lambda do |target, &block|
      target.send(@method_name, &block)
    end
  end
end
l = Call.new(:try).make_lambda
p l.call(5) { |x| x + 1 }
p l.call(Bag.new([1, 2])) { |x| x.size * 10 }
p l.call(Box.new([1, 2, 3])) { |x| x.size }
p Box.new([1, 2, 3]).try(:size)
p Bag.new([4]).try { |b| b.size + 100 }
