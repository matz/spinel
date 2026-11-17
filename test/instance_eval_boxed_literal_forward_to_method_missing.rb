# A literal in an instance_eval block on a boxed receiver, handed by the
# receiver's method to a Proxy's instance_eval: Proxy answers the literal's
# calls through its own method_missing, so it is still the literal's class.
class Proxy
  def initialize(source) = @source = source
  def method_missing(name, *args, &block) = @source.send(name, *args, &block)
end
class Event
  def initialize(&block)
    @t = []
    Proxy.new(self).instance_eval(&block) if block
  end
  def transitions(d) = @t << d
  def t = @t
end
class Base
  def initialize = @events = {}
  def events = @events
  def event(name, &block) = @events[name] = Event.new(&block)
end
class Other
end
b = Base.new
x = [b, Other.new][ARGV.size]
class Dsl
  def run(x)
    x.instance_eval do
      event :publish do transitions 1 end
    end
  end
end
Dsl.new.run(x)
p b.events[:publish].t
