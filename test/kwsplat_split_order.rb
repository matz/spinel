# The receiver, the positional operands and the `**` operand are evaluated once and in CRuby's order.
LOG = []
def rcv = (LOG << :r; [])
def arg = (LOG << :a; 1)
def kw = (LOG << :k; {})
def kwn = (LOG << :k; {x: 1})
p rcv.push(arg, **kw), LOG
LOG.clear
p rcv.push(arg, **kwn), LOG
LOG.clear
H = {}
def grow = (H[:g] = 1; 9)
p [].push(grow, **H)
H.clear
kv = {}
p [].push((kv = {z: 3}; 7), **kv)
r = [0]
p r.push((r = [5]; 1), **{}), r
