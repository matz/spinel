# MatchData#offset and #byteoffset of a group that did not take part in
# the match are [nil, nil]: each element reads as nil, through each, map,
# a multiple assignment and an index.
m = "a".match(/a(b)?/)
p m.offset(1).map { |v| v.nil? }
m.offset(1).each { |v| p v }
p m.byteoffset(1).map { |v| v.nil? }
s, e = m.offset(1)
p s, e
p m.offset(0), m.offset(0).sum
x = m.offset(1)[0]
p x
p(x.nil? ? :none : x + 1)
n = "xay".match(/(?<g>a)(?<h>b)?/)
p n.offset(:h).map(&:nil?), n.offset("g")
p n.byteoffset(:h).first
if "q" =~ /q(z)?/
  p $~.offset(1).first
end
