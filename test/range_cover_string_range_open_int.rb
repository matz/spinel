# Range#cover? of an Integer Range with a String Range argument: no Integer
# end compares with a String end, so only the missing ends of the receiver
# take the argument in, as CRuby's r_cover_range_p does.
p (nil..5).cover?(.."z")
p (nil...nil).cover?("a"..)
p (nil...nil).cover?("a".."b")
p (nil..nil).cover?("a"...)
p (1..).cover?("a"..)
p (nil..nil).cover?("a".."a")
p (nil..nil).cover?("a"..."a")
p (nil..5).cover?(..."z")
p (nil...5).cover?(.."z")
p (1..5).cover?("a".."b")
p (..5).cover?("a"..)
r = nil..nil
s = "a".."c"
p r.cover?(s)
e = ("b"..)
p r.cover?(e)
p (nil...nil).cover?(e)
def f(x) = (nil..nil).cover?(x)
[("a".."c"), (1..2), 3, ("c".."a"), (1.0..2.0), ("x"...)].each { |v| p f(v) }
def g(r, x) = r.cover?(x)
[(nil..nil), (1..3), (nil...nil)].each { |rr| p g(rr, "a".."b") }
