# slice!(x, n) with x a Regexp read out of a Hash cuts capture group n, as with a literal Regexp.
h = {r: /(or)/, i: 1, s: "x", g: /(?<w>l+)/, n: nil}
t = "hello world".dup
p t.slice!(h[:r], 1), t
t = "hello world".dup
p t.slice!(h[:r], 0), t
t = "hello world".dup
p t.slice!(h[:r], 2), t
t = "hello world".dup
p t.slice!(h[:i], 3), t
t = "hello world".dup
p((t.slice!(h[:s], 1) rescue $!.class), t)
t = "hello world".dup
p t.slice!(h[:g], 1), t
u = "nothing".dup
p u.slice!(h[:r], 1), u
def cut(s, re) = s.slice!(re, 1)
w = "abc123".dup
p cut(w, [/(\d+)/, 1][0]), w
t = "hello world".dup
p((t.slice!(h[:n], 1) rescue $!.class), t)
# The same call as a statement, its value unused.
h2 = {r: /(or)/, i: 1}
s = "hello world".dup
s.slice!(h2[:r], 1)
p s
s = "hello world".dup
s.slice!(/(or)/, 1)
p s
s = "hello world".dup
re = /(l+)/
s.slice!(re, 1)
p s
s = "hello world".dup
s.slice!(h2[:i], 2)
p s
s = "hello world".dup
s.slice!(1, 2)
p s
class K; def initialize = @s = "hello world".dup; def cut(x) = (@s.slice!(x, 1); @s); end
p K.new.cut(/(o)/)
[1].each { s = "abc".dup; s.slice!(/(b)/, 1) if true; p s }
