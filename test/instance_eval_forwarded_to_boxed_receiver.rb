# A literal handed through a method to `x.instance_eval(&block)`, where x
# is boxed (read from a Hash): the literal's own self calls pick the class
# it runs on, so its proc is made to run there and the inner literal it
# hands on is traced too.
class Proxy
  def initialize(source) = @source = source
  def transitions(d) = @source.transitions(d)
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
module Dsl
  def machine(&block)
    @machines ||= {}
    @machines[:default] ||= Base.new
    @machines[:default].instance_eval(&block) if block
    @machines[:default]
  end
end
class Article
  extend Dsl
  machine do
    event :publish do transitions 1 end
    # (an explicit self there is the receiver the block runs on too)
    self.event(:hide) { transitions 2 }
  end
end
p Article.machine.events[:publish].t
p Article.machine.events[:hide].t
