# String#equal? under --share-strings compares the identities the String
# plan gives both operands (#8328): a variable's handle, a box, a route's
# handle, a String Range's endpoint, an exception's message, or the bytes
# of a String no handle holds. The receiver is held while the argument runs.
a = +"a"; b = a; a << "1"
p a.equal?(b), !a.equal?(b), a.object_id == b.object_id
c = [a, 1][0]
p a.equal?(c), c.equal?(a), c.equal?(b.dup)
p String(a).equal?(a), (+a).equal?(b), a.itself.equal?(b)
p a.then { |v| v }.equal?(b), a.tap { }.equal?(b)
n = nil
n = +"x" if ARGV.size > 5
p n.equal?(nil), n.equal?(a), a.equal?(n)
x = "lit"; y = x
p x.equal?(y), x.equal?(+"lit")
def m(s) = s
p m(b).equal?(a), a.equal?(m(b))
def fresh = "z" * 2
p fresh.equal?(fresh), fresh.equal?((GC.start; fresh))
f = fresh; g = f
p f.equal?(g), g.equal?(f)
# a String Range's endpoints and an exception's message
r = a..(+"c")
p r.begin.equal?(a), a.equal?(r.begin), r.min.equal?(a), r.begin.equal?(r.first)
xs = [r.begin, r.end]
p xs[0].equal?(r.begin), xs[1].equal?(r.max)
e = RuntimeError.new(+"boom")
p e.message.equal?(e.message), e.to_s.equal?(e.message)
mm = e.message
p mm.equal?(e.message), [e, 1][0].message.equal?(mm)
# yields, a call spliced in with its block, a proc's answer, `&.`
LIT = "ghi"
def yield_it = yield
w = +"jk"; w << "l"
p yield_it { LIT }.equal?(LIT), yield_it { w }.equal?(w), w.equal?(yield_it { w })
u = +"q"
p yield_it { u }.equal?(u), yield_it { u }.equal?(w)
pr = proc { |k| k ? w : u }
p pr.call(true).equal?(w), pr.call(false).equal?(w)
s = +"ab"; t = s
p s.upcase!&.equal?(t), s.upcase!&.equal?(t)
v = nil
v = s if ARGV.empty?
p v&.equal?(t)
def give(o) = yield(o)
p give(s) { |z| z }.equal?(s)
h = { k: s }
p h[:k].equal?(s), s.equal?(h[:k]), [s, w].map { |q| q.equal?(w) }
# a route over a box answers the String the box holds; bangs and appends
# answer their receiver
nn = ARGV.size
cc = +"m#{nn}"
yy = nn == 0 ? cc : 5
p yy.to_s.equal?(cc), cc.equal?(yy.to_s), (+yy).equal?(cc)
ee = [RuntimeError.new(+"x"), 1][0]
p ee.to_s.equal?(ee.to_s), [cc, 2][0].to_s.equal?(cc)
q = +"abc"; qq = q
p q.upcase!.equal?(qq), (q << "d").equal?(qq), q.concat("e").equal?(qq), q.freeze.equal?(qq)
lo = +"a"; hi = +"z"; mid = +"m"
p mid.clamp(lo, hi).equal?(mid), (+"0").clamp(lo, hi).equal?(lo)
