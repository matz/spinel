# A constant that holds a Range (or a Proc, an Array, nil) in a pattern is
# Range#=== of the subject, which the pattern emitter does not test yet: it
# is refused, not folded to "no match" (or, for a path, to "always match").
module N
  R = (1..3)
end
p(case 2; in N::R then :r; else :no; end)
