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
