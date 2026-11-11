# `$~` after Regexp#match / String#match is that match.
# Regexp#match built its MatchData from positions of its own, and `$~` read
# the positions of an older match, so its groups came out as "".

m = /(a)(b)?(c)/.match("ac")
p $~
p m
p $~.to_a
p $~[1], $~[2], $~.pre_match, $~.post_match
def f(*a) = a
p f(*$~)
x = $~
p x.captures

"xay".match(/(a)/)
p $~
p $~.begin(0)
"bca".match(/(c)(a)/, 1)
p $~.to_a
p Regexp.last_match(2)
p Regexp.last_match
md = /(?<k>\w+)=(?<v>\w+)/.match("a key=val")
p $~[:k], $~[:v], Regexp.last_match(:v)
/(q)/.match("zz")
p $~
p $1

"ac" =~ /(a)(c)/
/(x)/.match("x")
p $~
p $1
