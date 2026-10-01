# A String mutated through the block parameter of an iterator over a
# builtin's Array of a container's own Strings (h.values, a.dup, a.select):
# CRuby mutates the String the container holds. The receiver was a call,
# not a local, so the container's Strings stayed values and the block
# appended to a copy.

def bang(s) = (s << "!"; nil)
def via(s) = bang(s)
h = {a: +"x", b: +"y"}
h.values.each { |v| v << "!" }
p h
m = {a: +"x", b: 1}
m.values.each { |v| v << "?" if v.is_a?(String) }
p m
hv = {a: +"p", b: +"q"}
hv.values.each { |v| via(v) }
hv.values.each_with_index { |v, i| v << i.to_s }
p hv
a = [+"a", +"b", +"c"]
a.dup.each { |s| s.upcase! }
a.select { |s| s != "B" }.each { |s| s << "-" }
a.sort.reverse.each { |s| s << "." }
p a
f = [+"f", +"g"]
f.map { |s| s.dup }.each { |s| s << "#" }
p f
hs = {"k" => +"v"}
hs.values.map { |v| v << "*"; v.size }
p hs
class Parts
  def initialize; @parts = {head: +"<", tail: +">"}; end
  def run
    @parts.values.each { |v| v << "#" }
    @parts
  end
end
p Parts.new.run
w = {a: +"w"}
w.values_at(:a).each { |v| v << "@" }
p w
