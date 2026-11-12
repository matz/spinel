# A Method made from an instance variable that holds a subclass is the
# class method the subclass inherits, or its own.

class Box
  def self.pick(value) = 17
  def pick(value) = "instance"
end

class Sub < Box
end

class Own < Box
  def self.pick(value) = 23
end

# a subclass stored in the ivar: the method it inherits, and one it overrides
class SubHolder
  def initialize
    @kind = Sub
  end

  def sub_loaded = @kind.method(:pick).call(nil)
  def sub_all(values) = values.map(&@kind.method(:pick))
  def sub_facts = [@kind.method(:pick).arity, @kind.method(:pick).owner, @kind.method(:pick).name]
end
p SubHolder.new.sub_loaded
p SubHolder.new.sub_all([1, 2])
p SubHolder.new.sub_facts

class OwnHolder
  def initialize
    @kind = Own
  end

  def own_loaded = @kind.method(:pick).call(nil)
  def own_facts = [@kind.method(:pick).arity, @kind.method(:pick).owner]
end
p OwnHolder.new.own_loaded
p OwnHolder.new.own_facts
