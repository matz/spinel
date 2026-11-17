# With an instance_exec the analysis cannot trace (Hook#run's @cb), a
# literal handed to a method takes a rebound self, typed boxed, and its
# bare calls a `self.` receiver. In a block the map runs on one class,
# `self.event { }` still forwards its literal to Proxy's instance_eval, and
# `self.transitions` there reaches Proxy's method_missing.
class Proxy0
end
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
module CM
  def aasm(&block)
    @aasm ||= {}
    @aasm[:default] ||= Base.new
    @aasm[:default].instance_eval(&block) if block
    @aasm[:default]
  end
end
class Article
  extend CM
  aasm do
    event :publish do transitions 1 end
  end
end
class Hook
  def initialize(&cb) = @cb = cb
  def run(o) = o.instance_exec(&@cb)
end
p Article.aasm.events[:publish].t
p Hook.new { self.class }.run(1)
