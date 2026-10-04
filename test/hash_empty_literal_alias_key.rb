# An empty Hash literal reached through another name -- a local it was
# assigned to, or an element of an Array literal -- takes the key kind
# that name is indexed with, as in CRuby. It kept the String-keyed
# default, and a Symbol or Integer key stored through the alias raised
# TypeError at run time.

x = {}; y = x; z = y; z[:a] = 1; p x
a = {}; b = a; b[1] = "one"; p a
c = {}; d = [c, 2][0]; d[:k] = :v; p c
e = {}; f = e; f.store(:s, 1); p e, f.key?(:s)
g = {"s" => 1}; h = g; h["t"] = 2; p g
class W
  def initialize = (@h = {})
  def put = (y = @h; y[:a] = 1; @h)
end
p W.new.put
