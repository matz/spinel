# A method an include or an extend copies into a class runs a call in its
# body against the version live when it runs: C.v is replaced between two
# calls of each copy, so the first call sees the first C.v and the second the
# second, as the module's own method would.
class C
  def self.v = 1
end
module M
  def b = C.v * 10
end
class D
  extend M
end
module N
  def w = C.v * 100
end
class E
  include N
  def w2 = w + 1
end
p D.b
p E.new.w2
def C.v = 2
p D.b
p E.new.w2
