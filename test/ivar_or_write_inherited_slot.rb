# A base whose only mention of an ivar is `@x ||= v` or `@x &&= v` reads the
# slot a subclass writes: the inherited method answers the subclass's value.
class Parent
  def memo
    @x ||= nil
  end

  def keep
    @y &&= :kept
  end
end

class Child < Parent
  def initialize
    @x = 42
    @y = "set"
  end
end

class Named < Parent
  def initialize
    @x = "name"
  end
end

p Child.new.memo
p Child.new.keep
p Named.new.memo
p Parent.new.memo
p Parent.new.keep
