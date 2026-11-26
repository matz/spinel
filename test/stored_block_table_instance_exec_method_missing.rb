# A block a class stores in a table by its &block, run by instance_exec or
# instance_eval on an object, runs on that object: a variable-like call in
# it reaches the object's method_missing. A block of a table run by call
# keeps its own self.
class Record
  def self.bodies
    @bodies ||= {}
  end

  def self.body(name, &block)
    bodies[name] = block
  end

  def initialize(attrs)
    @attrs = attrs
  end

  def method_missing(name, *args)
    return @attrs[name] if @attrs.key?(name)
    raise NoMethodError, "undefined method '#{name}'"
  end

  def respond_to_missing?(name, include_private = false)
    @attrs.key?(name)
  end

  def bare
    token
  end

  def run(name)
    self.instance_exec(&self.class.bodies[name])
  end
end

class Invitation < Record
  body :bare do
    token
  end
end

invitation = Invitation.new({ token: "abc" })
puts "token, in a method: #{invitation.bare}"
puts "token, in a stored block: #{invitation.run(:bare)}"

class Rec
  def self.bodies = (@bodies ||= {})
  def self.body(name, &block) = (bodies[name] = block)
  def self.label = "class-side"
  def initialize(attrs) = @attrs = attrs
  def method_missing(name, *args) = @attrs.key?(name) ? @attrs[name] : raise(NoMethodError, "undefined method #{name}")
  def respond_to_missing?(name, priv = false) = @attrs.key?(name) || super
  def run(name) = instance_exec(&self.class.bodies[name])
  def run_eval(name) = instance_eval(&self.class.bodies[name])
end
class Inv < Rec
  body(:a) { token }
  body(:b) { token() + "!" }
  body(:c) { [token, size] }
end
i = Inv.new({ token: "abc", size: 3 })
p i.run(:a), i.run(:b), i.run_eval(:c)
class Hooks
  def self.table = (@table ||= {})
  def self.on(name, &blk) = (table[name] = blk)
  def self.tag = "hooks"
  on(:x) { tag }
  def self.fire(name) = table[name].call
end
p Hooks.fire(:x)
