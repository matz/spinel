# A match in an ASCII-8BIT string reports its positions in bytes, so they
# agree with String#[] on the same string, whatever its bytes spell in UTF-8.
s = "café <%= x %> naïve <%= y %>".b
pos = 0
while (m = /<%=(.*?)%>/.match(s, pos))
  p [m[1], m.begin(0), m.end(0), s[pos, m.begin(0) - pos]]
  pos = m.end(0)
end
m = /<%=(?<code>.*?)%>/.match(s)
p m.offset(0), m.begin(:code), m.end("code"), m.byteoffset(0)
p(s =~ /y/)
p $~.begin(0)
p s.index(/</), s.index(/</, 7), s.rindex(/</), s.rindex(/</, 20)
s.scan(/<%=/) { p $~.begin(0) }
s.gsub(/%>/) { p Regexp.last_match.end(0); "" }
boxed = [/y/.match(s), 1].first
p boxed.begin(0)
# a UTF-8 string still counts characters
u = "café <%= x %> naïve <%= y %>"
p(/<%=(.*?)%>/.match(u).offset(0), (u =~ /y/), u.index(/</, 7), u.rindex(/</))
