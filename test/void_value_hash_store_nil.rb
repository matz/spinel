# `h[k] = nil` in value position (`return @cache[k] = nil`): the rhs is
# nil-typed and has no C storage type; the store temp must widen to poly
# instead of being declared `void _tN` (error: incomplete type 'void').
class Cache
  def initialize
    @h = {}
    @last = nil
  end

  def fetch(k)
    return @h[k] = nil
  end

  def put(k)
    @last = (@h[k] = nil)
  end

  def last
    @last
  end

  def size
    @h.size
  end
end

c = Cache.new
p c.fetch("a")
p c.put("b")
p c.last
p c.size
