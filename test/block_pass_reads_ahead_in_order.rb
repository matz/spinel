# Ruby evaluates a call's receiver and arguments before its block-pass
# expression. When that expression changes a variable an operand ahead of
# it only reads (a local it assigns, or an instance or global variable any
# code may change), the operand reads the value it had first.
def fb(a, &b) = b.call(a)
def fb2(a, b, &blk) = blk.call(a) + b

s = +"ab"
p fb(s, &(s = +"wxyz"; :upcase))
p s

class Box
  def initialize = @u = +"ef"
  def go
    p fb(@u, &(@u = +"yy"; :upcase))
    p @u
  end
end
Box.new.go

$g = +"gh"
def bump = ($g = +"zz"; :upcase)
p fb($g, &bump)

def run
  t = +"cd"
  p fb(t, &(t = +"zz"; :upcase))
  x = 5
  p [x].map(&(x = 9; :to_s))
  y = 1
  p fb2(y, y + 1, &(y = 100; :succ))
  n = [1, 2]
  p n.each_slice(1).map(&(n = []; :first))
  w = 7
  p fb(w, &(w += 1; proc { |v| v * 10 }))
  p w
end
run
