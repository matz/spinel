# A block made in a block instance_eval runs on a receiver of two classes,
# stored and later instance_exec'd: its fallback self is its maker's, taken
# where the block is made, and its receiverless calls run on the runtime self
class Event
  def initialize(&b)
    @log = []
    @b = b
  end
  attr_reader :log
  def transitions(**opts) = @log << opts
  def define(o) = o.instance_exec(&@b)
end
class Other
  def transitions(**opts) = :other
end
class Machine
  def initialize = @events = []
  attr_reader :events
  def event(name, &b) = @events << Event.new(&b)
end
class Machine2 < Machine
end
class Article
  def self.aasm(m, &b)
    m.instance_eval(&b)
    m
  end
  M = aasm(ARGV[0] ? Machine2.new : Machine.new) do
    event :publish do transitions from: :draft, to: :published end
  end
end
ev = Article::M.events[0]
ev.define(ev)
ev.define(Other.new)
p ev.log
