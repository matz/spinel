# A negative group index counts back from the last group of the match, as
# CRuby: s[re, -1], slice, slice! (also with a Regexp in a value), $~[n]
# and Regexp.last_match(n). One past the first group names none (nil).

s = "hello world"
re = /(or)(l)?/
p s[re, -1], s[re, -2], s[re, -3], s[re, -4], s.slice(re, -1)
h = {r: re}
p s[h[:r], -1], s[h[:r], -2], s[h[:r], -3]
u = s.dup
p u.slice!(re, -1), u
u = s.dup
p u.slice!(h[:r], -2), u
v = s.dup
v.slice!(h[:r], -1)
p v
"hello world" =~ re
p $~[-1], $~[-2], $~[-3]
i = -1
p $~[i]
p Regexp.last_match(-1), Regexp.last_match(-2), Regexp.last_match(i)
m = s.match(re)
p m[-1], m[-2], m[-3]
p "hello word"[/(or)(l)?/, -1]
n = -2
p s[re, n]
p "abc"[/(x)?b/, -1]

# a String in a boxed value
re3 = /(o)(r)(l)/
w = ["hello world", 5][0]
p w[re3, -1], w.slice(re3, -2), w[re3, -4]
x = [1, "hello world"].last
p x[/(o)(r)(l)/, -2]
