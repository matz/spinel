# A default of an inherited initialize runs on the object being built, as the
# class that defines initialize sees it: self.class is the subclass that
# called new, and self is that object. The default was typed in the defining
# class but read the subclass's own pointer, which the C compiler rejected.

class Parent
  attr_reader :kind
  def initialize(kind: self.class)
    @kind = kind
  end
  def self.build = new
end

class Child < Parent
end

class Other < Parent
end

class GrandChild < Child
end

puts Child.new.kind.name
puts Other.build.kind.name
puts GrandChild.new.kind.name
puts Parent.new.kind.name
puts Child.new(kind: Other).kind.name
[Parent, Child, Other, GrandChild].each { |k| puts k.new.kind.name }
puts Child.new.kind == Child

class ZSuper < Parent
  def initialize
    super
  end
end

class ArgSuper < Parent
  def initialize(k)
    super(kind: k)
  end
end

puts ZSuper.new.kind.name
puts ArgSuper.new(Child).kind.name

class Positional
  attr_reader :n, :kind
  def initialize(n, kind = self.class)
    @n = n
    @kind = kind
  end
end

class PositionalChild < Positional
end

puts PositionalChild.new(1).kind.name
puts PositionalChild.new(1, Positional).kind.name
[Positional, PositionalChild].each { |k| puts k.new(2).kind.name }
mixed = [PositionalChild, "x"]
puts mixed[0].new(3).kind.name

class Labeled
  attr_reader :label
  def self.label = name
  def initialize(label: self.class.label)
    @label = label
  end
end

class PlainLabeled < Labeled
end

class RenamedLabeled < Labeled
  def self.label = superclass.name
end

puts PlainLabeled.new.label
puts RenamedLabeled.new.label

class Node
  attr_reader :owner
  def initialize(owner)
    @owner = owner
  end
end

class Holder
  attr_reader :owner, :node, :items, :seed, :label
  def initialize(owner: self, node: Node.new(self), items: [self, describe],
                 seed: @seed, label: describe)
    @owner = owner
    @node = node
    @items = items
    @seed = seed
    @label = label
  end
  def describe = self.class.name
end

class SubHolder < Holder
end

class OtherHolder < Holder
  def describe = self.class.superclass.name
end

s = SubHolder.new
puts s.owner.equal?(s)
puts s.node.owner.equal?(s)
puts s.items[0].equal?(s)
puts s.items[1]
puts s.seed.nil?
puts OtherHolder.new.label
puts Holder.new.label

module Kinded
  attr_reader :kind
  def initialize(kind: self.class)
    @kind = kind
  end
end

class Base
  include Kinded
end

class Derived < Base
end

puts Derived.new.kind.name

objs = Array.new(20) { SubHolder.new }
puts objs.all? { |x| x.owner.equal?(x) && x.node.owner.equal?(x) && x.items[0].equal?(x) }
