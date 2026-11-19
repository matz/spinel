# A String Range keeps the shared String objects its ends were made from
# (--share-strings), so Marshal.dump writes each end under that object's
# identity: an end that is also an element of the same Array is written once
# and linked, as CRuby links it, and Marshal.load gives back the same objects.
t = String.new("k")
t << "x"
p Marshal.dump([t..t, t])
p Marshal.dump([t, t..t])
p Marshal.dump(t..t)
u = Marshal.load(Marshal.dump([t..t, t]))
p u[0].begin.equal?(u[1]), u[0].end.equal?(u[1]), u[0].begin.equal?(u[0].end)
c = String.new("zz")
c << "y"
r = (c.."zzz")
p Marshal.dump([c, r, r.end])
literal_end = Marshal.load(Marshal.dump([r, r.end]))
p literal_end[0].end.equal?(literal_end[1])
reversed_end = Marshal.load(Marshal.dump([r.end, r]))
p reversed_end[1].end.equal?(reversed_end[0])
distinct_end = Marshal.load(Marshal.dump([r, r.end.dup]))
p distinct_end[0].end.equal?(distinct_end[1])
a = String.new("m")
a << "m"
b = String.new("n")
b << "n"
p Marshal.dump([a...b, b, a])
v = Marshal.load(Marshal.dump([a...b, b, a]))
p v[0].begin.equal?(v[2]), v[0].end.equal?(v[1]), v[0].begin.equal?(v[0].end)
