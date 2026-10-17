# match and match? from a position, !~ and =~, on a Regexp read out of a
# Hash (a boxed operand), diffed against CRuby: the receiver's or the
# argument's side may be the boxed one. match(re, pos) raised
# NoMethodError, as did !~, and the call's MatchData was typed nothing.
t = { re: /b+/, name: "x" }
re = t[:re]
p re.match("abbcb")[0]
p re.match("abbcb", 2)[0]
p re.match("abbcb", 4)[0]
p re.match("abbcb", 5)
p "abbcb".match(re, 2)[0]
p re.match?("abbcb", 4)
p "abc" !~ re
p "xyz" !~ re
p "abc" =~ re
x = "abc" !~ re
p x
"xbb" =~ /b/
p re.match("abbcb", 9)
p $~
log = []
p((log << "recv"; "abc") !~ (log << "arg"; re))
p((log << "recv"; "abc") =~ (log << "arg"; re))
p log
