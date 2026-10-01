# Stores that each convert (or need nothing): --check-stores reports none.
class Base; end
class Kid < Base; end
@obj = Base.new
@obj = Kid.new
x = 1.5
x = 2
a = [1, 2]
a[0] = 3
a << 4
h = { "a" => 1 }
h["b"] = 2
$g = 2**64
$g = $g + 1
s = "s"
s = 1 if a.size > 9
p @obj.class, x, a, h, $g, s
