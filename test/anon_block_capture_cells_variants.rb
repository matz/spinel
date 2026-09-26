# An anonymous `&` forward behaves as a named one: the stored block captures
# the caller's locals by cell, keeps its own self (ivar reads and writes,
# implicit-self calls), survives two levels of forwarding, and a method
# taking an anonymous `&` is a real method with CRuby's #parameters.

class Register
  def handle(&handler) = @handler = handler
  def poke(value) = @handler.call(value)
end

class Bus
  attr_reader :register
  def initialize
    @register = Register.new
    @tag = :bus
  end
  def install(&) = @register.handle(&)
end

class Hub
  attr_reader :bus
  def initialize = @bus = Bus.new
  def install(&) = @bus.install(&)
end

class Recorder
  attr_reader :log
  def initialize
    @log = []
    @tag = :recorder
  end
  def note(v) = @log << v
  def wire(bus, hub)
    bus.install { |v| @log << [:anon, @tag, v] }
    bus.register.poke(1)
    bus.install { |v| note([:implicit_self, v]) }
    bus.register.poke(2)
    hub.install { |v| @log << [:hub, @tag, v] }
    hub.bus.register.poke(3)
  end
end

r = Recorder.new
r.wire(Bus.new, Hub.new)
r.log.each { |e| p e }

total = 0
count = 0
bus = Bus.new
bus.install { |v| total += v; count += 1 }
bus.register.poke(5)
bus.register.poke(6)
p [total, count]

hub = Hub.new
seen = []
hub.install { |v| seen << v }
hub.bus.register.poke(:x)
p seen

class K
  def m(a, &) = a
end
p K.instance_method(:m).parameters
def given(&) = block_given?
def pass_on(&) = given(&)
p [pass_on { }, pass_on]
