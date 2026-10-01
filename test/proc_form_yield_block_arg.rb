# A method that hands its block on (`@parts.each(&blk)`) yields the Strings
# to the blocks its own callers pass. A lambda elsewhere that appends to what
# it is handed is never one of them, so it does not make the yield refuse.
class EachBody
  def initialize(parts) = @parts = parts
  def each(&blk) = @parts.each(&blk)
end

class Out
  def initialize = @buf = []
  # Retaining a POLY String via an alias is not mutating that String.
  # The identity demand must not turn this readonly callback into an appender.
  def <<(s)
    s = s
    @buf.push(s)
    self
  end
  def text = @buf.join
end

def write(body, out)
  if body.respond_to?(:to_ary) || !body.respond_to?(:call)
    body.each { |x| out << x }
  else
    body.call(out)
  end
  out.text
end

p write(EachBody.new(["a", "b"]), Out.new)
p write(->(s) { s << "c" }, Out.new)

# the same through a Hash
class PairBody
  def initialize(h) = @h = h
  def each(&blk) = @h.each(&blk)
end
seen = []
PairBody.new({ "k" => "v" }).each { |k, v| seen << "#{k}=#{v}" }
p seen
add = ->(s) { s << "!" }
p add.call(+"x")
