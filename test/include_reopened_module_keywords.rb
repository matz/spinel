# A module included by a reopening of a class, of a method the class does
# not have, is copied with the module method's own parameters: the copy
# was made from the scope table as it stood before the include made a
# fallback scope, and a keyword parameter could come out a positional one
# (`a(k: 4)` answered `{k: 4}`).
class Pad
  def p0 = 0
  def p1 = 1
  def p2 = 2
  def p3 = 3
  def p4 = 4
end
module M
  def a(k: 1) = k
end
class C; def z = 0; end
class C; include M; end
p C.new.a
p C.new.a(k: 4)
