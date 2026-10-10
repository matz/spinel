# A Data or Struct member argument that can be nil and runs code (a call)
# keeps its nil: the constructor took it plain and raised TypeError. It is
# held with its nil, and so is every argument with a side effect ahead of
# it, so the arguments still run in their source order ($log).
D = Data.define(:a, :b)
def r(x) = x.zero? ? nil : x
$log = []
def t(v) = ($log << v; v)
p D.new(a: r(0), b: 1)
p D.new(a: r(2), b: 3)
p D.new(b: t(5), a: t(r(0)))
p $log
S = Struct.new(:x, :y)
p S.new(t(1), r(0))
