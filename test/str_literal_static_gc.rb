# spinel: gc-minor
# spinel: gc-stress
# A frozen literal's String is a static object the collector never
# allocated: its header says old, frozen and pinned and links to itself.
# Here literals sit in every kind of holder -- an ivar of objects that are
# old by then, a typed String Array, a poly Array, a String-keyed Hash, an
# Integer => String Hash, a constant, a Struct -- across minor and major
# collections. A literal stored into an old holder takes no write barrier
# (it can never be young), so the generational verifier in the gc-minor leg
# reports a holder whose young value went unrecorded if the barrier was
# dropped from the wrong store; the gc-stress leg runs it under
# SPINEL_GC_VERIFY, whose registry must take the static header.
class Holder
  attr_accessor :s, :t, :kid
  def initialize
    @s = "ivar-lit"
    @t = nil
    @kid = nil
  end
end

TAG = "const-lit"
Pair = Struct.new(:a, :b)

def lit = "method-lit"

olds = []
100.times { olds << Holder.new }
GC.start
arr = []
poly = [1, :a]
keys = {}
vals = {}
pair = Pair.new(nil, nil)
3.times { GC.start }
600.times do |i|
  junk = Array.new(10) { |k| (k * i).to_s }
  o = olds[i % 100]
  o.t = "late-lit"
  o.kid = junk if i % 50 == 0
  arr << lit if i % 60 == 0
  poly << "poly-lit" if i % 60 == 0
  keys["key-lit"] = i
  vals[i % 7] = "val-lit"
  pair.a = "struct-lit"
  GC.start if i % 150 == 0
end
GC.start
p arr.size, arr.all? { |x| x.equal?(lit) }, arr[0].frozen?
p poly.size, poly[2].equal?(poly[-1])
p keys, vals.values.uniq, pair.a.equal?(pair.a)
p olds.map(&:t).uniq, olds.map(&:s).uniq, olds[0].s.equal?(olds[99].s)
p olds.all? { |o| o.t.equal?(olds[0].t) }, olds.count { |o| o.kid }
p TAG.equal?(TAG), TAG.frozen?
x = arr[0]
GC.start
p x.object_id == lit.object_id, x.equal?(lit)
s = +lit
s << "!"
p s, lit
