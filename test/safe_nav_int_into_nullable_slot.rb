# spinel: int64
# `r&.to_i` stored into a nullable Integer slot is nil when r is nil, even in
# a program that names an Integer near 2^63 (which turns on the -2^63 store
# check): the check must not mistake the safe-navigation nil for -2^63.
class R
  def initialize; @x = nil; end
  def x; @x; end
  def x=(value); @x = value; end
end
row = { "a" => "7", "b" => nil, "c" => 3 }
r = R.new
r.x = row["b"]&.to_i
p r.x
r.x = row["a"]&.to_i
p r.x
@y = 1
@y = row["b"]&.to_i
p @y
p 4611686018427387904
