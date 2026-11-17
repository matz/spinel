# A call no method answers, on a value whose class (as the program's flow
# knows it) defines method_missing, calls method_missing with the name.
class Event
  def transitions(opts) = (@t = opts)
  attr_reader :t
end
class Proxy
  def initialize(src)
    @src = src
  end
  def method_missing(name, *args, &b) = @src.send(name, *args, &b)
  def run = transitions(from: :draft)
end
module Helper
  def add_opts(&block)
    Proxy.new(self).instance_eval(&block)
  end
end
class Flow < Event
  include Helper
  def initialize(&block)
    add_opts(&block) if block
  end
end
class Ghost
  def method_missing(name, *args) = "ghost #{name} #{args.inspect}"
end
e = Event.new
p Proxy.new(e).transitions(x: 1)
Proxy.new(e).run
p e.t
Proxy.new(e).instance_eval { transitions(y: 2) }
p e.t
p Flow.new { transitions(from: :a, to: :b) }.t
g = Ghost.new
p g.anything(1, 2)
p g.to_s.start_with?("#<Ghost")
objs = [Ghost.new, e]
o = objs[ARGV.size]
p o.t
class Parent
  def method_missing(name, *args) = "parent mm #{name}"
  def run = foo
end
class Child < Parent
  def foo = "child foo"
end
pc = [Parent.new, Child.new]
a = pc[ARGV.size]
b = pc[ARGV.size + 1]
p a.foo
p b.foo
p Parent.new.run
p Child.new.run
class Probe
  def method_missing(name, *args) = :mm
  def probe = defined?(foo)
  def instance_of?(k) = false
end
p Probe.new.probe
pr = [Probe.new, e][ARGV.size]
p pr.foo
class Slot
  def method_missing(name, *args) = :handled
end
sl = [Slot.new, {}][ARGV.size]
p(sl[0] = 7)
