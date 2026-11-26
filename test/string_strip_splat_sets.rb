# String#strip, lstrip, rstrip and their bang forms take their character
# sets from a splat: none (whitespace), one set, or several (their
# intersection); an element that is no String raises TypeError
none = []
one = ["x"]
two = ["xy", "y"]
p " b ".strip(*none)
p "xxbxx".strip(*one)
p "yxbxy".strip(*two)
p "  b ".lstrip(*none), "xxb".lstrip(*one), "bxx".rstrip(*one)
s = +"xbx"
p s.strip!(*one), s
t = +"b"
p t.strip!(*one)
u = +"xxb"
p u.lstrip!(*one), u
begin
  "b".strip(*[1])
rescue TypeError => e
  puts e.message
end

def trim(*) = "xbx".strip(*)
p trim("x"), trim
sets = "x"
p "xbx".strip(*sets)
p " b ".strip(*nil)
class XSet
  def to_str = "x"
end
p "xbyx".strip(*[XSet.new, "xy"])

# delete, count and squeeze gather their sets the same way when the
# count is the run time's (two splats); delete and count take one at least
class Sets
  def initialize(a, b) = (@a = a; @b = b)
  def del(str) = str.delete(*@a, *@b)
  def cnt(str) = str.count(*@a, *@b)
  def sq(str) = str.squeeze(*@a, *@b)
end
p Sets.new(["xy"], ["y"]).del("xyz"), Sets.new(["l"], []).cnt("hello"), Sets.new([], []).sq("xxyy  ")
begin
  Sets.new([], []).del("xy")
rescue ArgumentError => e
  puts e.message
end
begin
  Sets.new([], []).cnt("xy")
rescue ArgumentError => e
  puts e.message
end

class Proxy
  def initialize(body) = @body = body
  def method_missing(name, ...) = @body.__send__(name, ...)
  def respond_to_missing?(*) = true
end
p Proxy.new(+" b ").strip!
p Proxy.new(+"xbx").strip("x")
