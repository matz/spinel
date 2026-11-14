# A String operand held across a sibling that changes it in place and then
# rebinds its variable is the String the operand named: the call sees the
# change and not the new String. Holding the operand's bytes kept neither
# (the change moved the payload under them); reading the variable last took
# the new String. A sibling that only rebinds the variable leaves the old
# String, as before, also where the call hands the operand on as its String
# (`p a, b` whose value is used) and where the rebinding runs in an
# interpolation's implicit to_s.

def run
  s = +"ab"; g = proc { s << "zz"; s = +"WXYZ"; "-" }
  p s + g.call
  s = +"ab"; g = proc { s << "a"; s = +"WXYZ"; "Q" }
  p s.gsub("a", g.call)
  s = +"%s!"; g = proc { s << "%s"; s = +"WXYZ"; ["a", "b"] }
  p s % g.call
  s = +"ab"; g = proc { s << "c"; s = +"WXYZ"; 2 }
  p s * g.call
  s = +"ab"; g = proc { s << "zz"; s = +"WXYZ"; "-" }
  p "q".center(12, s + g.call)
  s = +"ab"; g = proc { s << "zz"; s = +"WXYZ"; "-" }
  p s.center(10, g.call)
  s = +"ab"; g = proc { s = +"WXYZ"; "-" }
  p s + g.call
  s = +"ab"
  p s.center(8, (s = +"WXYZ"; "-"))
  s = +"ab"
  p [s, s.center(8, (s << "zz"; s = +"WXYZ"; "-")), s]
end
run

class Both
  def initialize = @s = +"ab"
  def both = (@s << "zz"; @s = +"WXYZ"; "-")
  def reas = (@s = +"WXYZ"; "-")
  def go
    p @s.center(10, both)
    @s = +"ab"
    p @s.center(10, reas)
  end
end
Both.new.go

# p's value is the Array of its operands, each the object it was given
def pv
  s = +"ab"
  p s, (s = +"XY"; "q")
end
pv
def pg
  s = +"ab"; g = proc { s = +"XY"; "q" }
  x = p(s, g.call)
  x
end
p pg

class O
  def initialize(&b) = (@b = b)
  def to_s
    @b.call
    "o"
  end
end
def tos
  s = +"ab"; o = O.new { s = +"XY" }
  p s.center(10, "#{o}")
  s = +"ab"; o = O.new { s = +"XY" }
  p s.center(8, ("#{o}"; "q"))
  s = +"ab"; o = O.new { s << "zz"; s = +"XY" }
  p s.center(10, "#{o}"), s
end
tos
s = +"ab"; o = O.new { s = +"XY" }
p s.center(8, "#{o}")
