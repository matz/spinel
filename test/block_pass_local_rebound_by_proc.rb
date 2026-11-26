# A block-pass expression that runs a proc rebinding a local the arguments
# read: the arguments read the local before the block expression runs, as
# CRuby evaluates them in order.
def fb(a, &b) = b.call(a)
def fb2(a, b, &blk) = blk.call(a) + b

def bp
  s = +"ab"
  g = proc { s = +"WXYZ"; :upcase }
  p [fb(s, &g.call), s]
end
def bp2
  s = +"ab"; t = +"cd"
  g = proc { s = +"WXYZ"; t = +"Q"; :upcase }
  p [fb2(s, t, &g.call), s, t]
end
bp; bp2
