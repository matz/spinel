# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A bound Method whose return is a nullable Integer (nil on a miss, -2**63 on
# a hit) answers both through call, to_proc, map(&m), curry and unbind/bind.
def find(a, v) = a.index(v)
def minv(n) = n > 100 ? nil : -9223372036854775807 - (n + 1)
def minf(n) = n > 100 ? nil : ("\x01\x00\x00\x00\x00\x00\xF8\x7F" * (n + 1)).unpack1("d")
class Finder
  def initialize(a) = @a = a
  def pos(v) = @a.index(v)
  def low(n) = n > 100 ? nil : -9223372036854775807 - (n + 1)
end
n = ARGV.size
bf = method(:find)
p bf.call([1, 2], 2)
p bf.call([1, 2], 9)
p bf.call([1, 2], 9).nil?
p bf.([1, 2], 1)
p bf[[1, 2], 7]
p bf.to_proc.call([3, 4], 4)
p bf.curry[[5, 6]][6]
p bf.curry[[5, 6]][8]
bm = method(:minv)
p bm.call(n)
p bm.call(1000)
p bm.call(n).nil?
p bm.call(n) ? 1 : 2
p [n, 1000].map(&bm)
p [n, 1000].map(&bm).compact
p [n, 1000].map(&bm).first
p bm.to_proc.call(n)
p bm.arity, bm.name
p bm.unbind.bind(self).call(n)
r = bm.call(n)
p r
p r.nil?
puts "#{r}"
bg = method(:minf)
p bg.call(n)
p bg.call(1000)
p bg.call(n).nan?
f = Finder.new([1, 2])
fp = f.method(:pos)
p fp.call(2), fp.call(9)
fl = f.method(:low)
p fl.call(n), fl.call(1000)
p [n].map(&fl)
p Finder.instance_method(:low).bind(f).call(n)
p fl.owner, fl.receiver.equal?(f)
h = { n => 1 }
p h.map { |k, _| bm.call(k) }
p h.transform_keys(&bm)
