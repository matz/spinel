# A rescued exception is boxed under the builtin exception id, not its class's
# id: after e.is_a?(K) narrows a poly e to a user exception class K (one with
# no subclass), reading e as a K takes that box too, instead of raising
# "wrong argument type K (expected K)"

class Slow < StandardError
  attr_reader :wait
  def initialize(msg = "slow", wait = 2)
    super(msg)
    @wait = wait
  end
  def message = "slow: #{super}"
end
class Bad < StandardError; end
# under another builtin, and a leaf under a class with ivars, raised by name
class Closed < IOError
  def initialize(msg = "closed") = super
end
class Base < StandardError
  attr_reader :code
  def initialize(msg = "base")
    super
    @code = 7
  end
end
class Leaf < Base
  attr_reader :items
  def initialize(msg = "leaf")
    super
    @items = [1, 2, 3]
  end
end

def answer(e)
  if e.is_a?(Slow)
    "429 #{e.wait} #{e.message} #{e.class} #{e.cause ? e.cause.message : "-"}"
  else
    "500 #{e.class}: #{e.message}"
  end
end

def guarded(e)
  return "500 #{e.message}" unless e.kind_of?(Slow)
  "429 #{e.wait}"
end

def wait_of(s) = s.wait

def passed(e)
  e.is_a?(Slow) ? "429 #{wait_of(e)}" : "500"
end

def other(e)
  if e.is_a?(Closed) then "closed #{e.message}"
  elsif e.is_a?(Leaf) then "leaf #{e.code} #{e.items.sum} #{e.message}"
  else "other #{e.class}"
  end
end

def work(n)
  case n
  when 0 then raise Slow.new("built", 3)
  when 1 then raise Slow, "named"
  when 2
    begin
      raise ArgumentError, "inner"
    rescue
      raise Slow.new("wrapped", 4)
    end
  when 3 then raise Bad, "bad"
  when 4 then raise KeyError, "key"
  when 5 then raise Closed, "named closed"
  else raise Leaf, "named leaf"
  end
end

def call(n)
  work(n)
rescue => e
  [answer(e), guarded(e), passed(e), other(e)].join(" / ")
end

# a direct call with each class makes the parameters poly
puts [answer(Slow.new("direct", 1)), guarded(Slow.new("direct", 1)), passed(Slow.new("direct", 1))].join(" / ")
puts [answer(Bad.new("direct")), guarded(Bad.new("direct")), passed(Bad.new("direct"))].join(" / ")
puts other(Closed.new), other(Leaf.new), other(Base.new)
7.times { |i| puts call(i) }
