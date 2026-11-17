# A prepended initialize takes the call's positional arguments by count and
# its keywords in its keyword rest, and a bare `super` passes both on to
# Hash's own initialize, which takes the keywords. Without a keyword rest
# the keywords are one more positional Hash, which Array's initialize takes
# as one too. The call's operands run once each, in order.
module HashOne
  def initialize(d, **kw)
    super
  end
end
module HashExplicit
  def initialize(d, **kw, &b)
    super(d, **kw, &b)
  end
end
Hash.prepend(HashOne)
Hash.prepend(HashExplicit)
log = []
h = Hash.new((log << :default; 5), capacity: (log << :capacity; 3))
p h[:x], log
opts = { capacity: 4 }
g = Hash.new(:z, **opts)
p g[:q], g.size

module ArrayPair
  def initialize(n, fill) = super
end
module ArrayRest
  def initialize(*a) = super
end
Array.prepend(ArrayPair)
Array.prepend(ArrayRest)
a = Array.new((log << :size; 2), k: (log << :k; 1))
p a, log
