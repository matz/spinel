# A String a Hash's default block stores (or its default value), mutated
# through a read of a missing key: CRuby mutates the String the Hash holds.
# The default block's stores were not walked as the Hash's, and the
# mutation landed in a copy.

def bang(s) = (s << "!"; nil)
def via(s) = bang(s)
h = Hash.new { |hh, k| hh[k] = +"d" }
h[:q] << "!"
h[:q] << "?"
h[:r].upcase!
p h
sh = Hash.new { |hh, k| hh[k] = String.new }
sh["a"] << "x"
sh["a"] << "y"
sh["b"].concat("z")
p sh
kh = Hash.new { |hh, k| hh[k] = k.to_s * 2 }
kh[:ab] << "!"
via(kh[:cd])
p kh
ih = Hash.new { |hh, k| hh[k] = "#{k}-" }
ih[1] << "one"
p ih
ms = Hash.new { |hh, k| s = +"m"; hh[k] = s; s }
ms[:a] << "!"
p ms
nostore = Hash.new { |_hh, _k| +"n" }
nostore[:a] << "!"
p nostore
ah = Hash.new { |hh, k| hh[k] = [] }
ah[:a] << 1
ah[:a] << 2
p ah
dv = Hash.new(+"d")
dv[:q] << "!"
p dv, dv[:z]
class Log
  def initialize; @by = Hash.new { |hh, k| hh[k] = +"" }; end
  def add(k, s) = (@by[k] << s; self)
  def show = p(@by)
end
Log.new.add(:a, "x").add(:a, "y").add(:b, "z").show
