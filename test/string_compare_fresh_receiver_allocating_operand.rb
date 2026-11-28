# spinel: gc-stress
# A fresh String compared with <, <=, >, >= or <=> against a parenthesized
# operand that allocates: the receiver is held rooted while the operand runs.
# A collection there freed it before the compare, and the answer read freed
# bytes (false where CRuby answers true, under SPINEL_GC_STRESS=1).
keep = Array.new(100) { |i| "s#{i}" }
p("b".dup < ("abc".scan(/\w/).join))
p("b".dup < "abc".scan(/\w/).join)
p("b".dup <= ("abc".scan(/\w/).join))
p("b".dup > ("abc".scan(/\w/).join))
p("b".dup >= ("abc".scan(/\w/).join))
p("b".dup <=> ("abc".scan(/\w/).join))
p(("abc".scan(/\w/).join) > "b".dup)
p keep.size
