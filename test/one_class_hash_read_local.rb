# A hash filled only with one class's objects reads that class (#4846), and
# a local the read is kept in (`if n = @h[k]`, `n = @h[k]`) holds it too, a
# miss as nil: passed on, it types what it reaches. Here Lru's @head and
# Nd's @nxt hold an Nd, and the hash is String-keyed (the key read off an
# Nd is a String). A hash of mixed values still boxes such a local.
class Nd
  attr_accessor :key, :nxt
  def initialize(key) = (@key = key; @nxt = nil)
end
class Lru
  attr_reader :head
  def initialize = (@h = {}; @head = nil)
  def touch(n) = (n.nxt = @head; @head = n)
  def put(k)
    if n = @h[k]
      touch(n)
      return :hit
    end
    n2 = Nd.new(k)
    @h[k] = n2
    touch(n2)
    :miss
  end
  def get(k)
    n = @h[k]
    n ? n.key : nil
  end
  def drop = (o = @head; @h.delete(o.key) if o; o && o.key)
  def size = @h.size
end
l = Lru.new
p %w[a b a c].map { |k| l.put(k) }
p l.get("a"), l.get("zz"), l.head.key, l.head.nxt.key
p l.drop, l.size

mixed = {"n" => Nd.new("x"), "i" => 3, "f" => false}
%w[n i f none].each do |k|
  v = mixed[k]
  p(v.is_a?(Nd) ? v.key : v)
end
