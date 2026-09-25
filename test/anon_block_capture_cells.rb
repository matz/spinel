# A block handed through an anonymous `&` to a method that stores it keeps
# writing the caller's locals. The forwarder was always yield-inlined (an
# anonymous param had no name for the escape analysis to follow), so the
# block became a proc inside the splice with its captures copied by value:
# `total` stayed 0.

class Reg
  def set(&h) = @h = h
  def poke(v) = @h.call(v)
end
R = Reg.new
def install(&) = R.set(&)
total = 0
install { |v| total += v }
R.poke(3)
R.poke(4)
p total
