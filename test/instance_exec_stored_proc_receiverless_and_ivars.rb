# A stored block run with instance_exec reads its rebound self through a
# receiverless call (a public method, a private one, respond_to?) and through
# its ivars, as Ruby's does; called as written, the same block reads the self
# it was made in.
class Machine
  def initialize
    @callbacks = []
  end

  def after_all(&block)
    @callbacks << block
  end

  def run(record)
    @callbacks.map { |cb| record.instance_exec(&cb) }
  end

  def call_plain
    @callbacks.map(&:call)
  end
end

class Rec
  def initialize
    @name = "rec"
    @count = 0
  end
  attr_reader :count

  def title
    "rec-title"
  end

  def label
    "#{secret}!"
  end

  private

  def secret
    "s"
  end
end

class Host
  def initialize
    @name = "host"
    @count = 10
  end
  attr_reader :count

  def title
    "host-title"
  end

  def secret
    "host-s"
  end

  def build
    m = Machine.new
    m.after_all { title }
    m.after_all { secret }
    m.after_all { respond_to?(:label) ? label : "none" }
    m.after_all { @name }
    m.after_all { @count = @count + 1 }
    m
  end
end

h = Host.new
m = h.build
r = Rec.new
p m.run(r)
p r.count
p m.call_plain
p h.count
