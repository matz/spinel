# spinel: share
# spinel: gc-stress
# MatchData#values_at selects groups by index (nil for a group that did not
# participate); #byteoffset/#bytebegin/#byteend report raw byte positions (the
# char-based #begin/#end/#offset already worked). The MatchData receiver is
# routed through a method param.
def md(x); x; end

m = md("a1b2c3".match(/(\d)(\w)(\d)/))
p m.values_at(1, 3)
p m.values_at(0, 2)
p m.byteoffset(0)
p m.byteoffset(2)
p m.bytebegin(1)
p m.byteend(1)

# a non-participating optional group yields nil in values_at and in the
# byte-offset accessors (mirroring the char-based begin/end/offset)
n = md("x".match(/(\d)?(x)/))
p n.values_at(1, 2)
p n.bytebegin(1)
p n.byteend(1)
p n.byteoffset(1)

# Offset accessors reject missing indices, rather than returning nil pairs.
[5, -1].each do |i|
  begin
    p m.byteoffset(i)
  rescue IndexError => e
    p e.message
  end
end
