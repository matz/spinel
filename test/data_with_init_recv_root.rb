# Data#with keeps the receiver and its `**hash` alive while the
# constructor's arguments run: a keyword default of initialize, or the call
# that makes the `**hash`, allocates after a receiver made in place and
# before its members are read, which SPINEL_GC_STRESS=2 shows.
# spinel: gc-stress
Q = Data.define(:a, :b)
class Q
  def initialize(l: [], a:, b:)
    super(a: a, b: b + l.size.to_s)
  end
end
P = Data.define(:a, :b)

def mq(i) = Q.new(a: "a" + i.to_s, b: "b")
def mp(i) = P.new(a: "a" + i.to_s, b: "b")
def hb(i) = {b: "h" + i.to_s}

bad = 0
300.times do |i|
  x = mq(i).with(b: "c")
  bad += 1 unless x.a == "a#{i}"
  y = mq(i).with(**hb(i))
  bad += 1 unless y.a == "a#{i}" && y.b == "h#{i}0"
  z = mp(i).with(**hb(i))
  bad += 1 unless z.a == "a#{i}" && z.b == "h#{i}"
end
p bad
p mq(1).with(b: "x"), mq(2).with(**hb(2)), mp(3).with(**hb(3))
