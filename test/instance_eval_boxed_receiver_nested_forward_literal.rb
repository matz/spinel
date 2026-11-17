# A literal block inside an instance_eval block on a boxed receiver,
# handed by the receiver's method to another instance_eval: the typed
# arm's retype of the outer block keeps the inner literal on the class it
# is forwarded to, so its proc is made to run there.
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
  def tag = :base
end
h = { a: Base.new }
b = h[:a]
h[:a].instance_eval do
  event :publish do transitions 1 end
end
p b.events[:publish].t
# a receiverless instance_eval in the same block still runs on the arm's self
p(h[:a].instance_eval { instance_eval { tag } })
