# A reopened builtin exception's initialize that asks for, takes or yields to its block
class IndexError
  def initialize(m = nil, &blk)
    @seen = blk.nil? ? "no block" : "block"
    super("I:" + m.to_s)
  end
  attr_reader :seen
end
class KeyError
  def initialize(m = nil)
    yield m if block_given?
    super("K:" + m.to_s)
  end
end
class StandardError
  def initialize(m = nil)
    @bg = block_given?
    super("S:" + m.to_s)
  end
  def bg = @bg
end
e = IndexError.new("k")
p e.message, e.seen
e = IndexError.new("k") { 1 }
p e.message, e.seen
b = proc { 2 }
e = IndexError.new("k", &b)
p e.message, e.seen
p KeyError.new("a") { |x| puts "yielded #{x}" }.message
p KeyError.new("b").message
begin
  raise IndexError, "r"
rescue IndexError => x
  p x.message, x.seen
end
begin
  raise IndexError
rescue IndexError => x
  p x.message, x.seen
end
begin
  raise KeyError, "q"
rescue KeyError => x
  p x.message
end
begin
  [1].fetch(5)
rescue IndexError => x
  p x.class
end
e = ArgumentError.new("z") { 1 }
p e.message, e.bg
e = ArgumentError.new("z")
p e.message, e.bg
