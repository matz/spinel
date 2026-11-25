# spinel: gc-stress
# A block scan over a String pattern walks matches it computed up front and
# answers its subject after the last turn; the subject may be a temporary
# nothing else holds, so it is rooted across the turns, whose allocations
# (and polls) may collect.
def go(n)
  r = ("ab" * n + "c").scan("a") { |m| $junk = m + "z" * 50 }
  s = "x#{n}ab".scan("a") { |m| $junk = m * 3 }
  r.length + s.length
end

t = 0
60.times { |i| t += go(i + 1) }
p t
