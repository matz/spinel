# spinel: share
# spinel: gc-stress
# Marshal.dump writes a Range as CRuby does, an `o` record of class Range with
# its exclude-end flag and its two ends, and Marshal.load reads one back as a
# Range of the same kind: String, Float, or Integer (absent ends as nil).
def round(r)
  d = Marshal.dump(r)
  p d
  x = Marshal.load(d)
  p x.begin, x.end, x.exclude_end?, x.class, x == r, x.frozen?
end
round("a".."c")
round("a"..."c")
round("a"..)
round(.."z")
round(String.new("ab")..String.new("ad"))
round(1..3)
round(1...3)
round(1..)
round(..5)
round(-1..-5)
round(1.0..2.5)
round(1.0...2.5)
round(1.5..5)
round(1..2.5)
round(1.0..)
round(..2.5)
# links: the String ends of Ranges that share, or do not share, an object
p Marshal.dump(["a".."b", "a".."b"])
s = String.new("zz")
p Marshal.dump([s..s, s])
a = String.new("m")
b = String.new("n")
p Marshal.dump([a...b, b, a])
v = Marshal.load(Marshal.dump([a...b, b, a]))
p v[0].begin.equal?(v[2]), v[0].end.equal?(v[1]), v[0].begin.equal?(v[0].end)
z = Marshal.load(Marshal.dump([1..2, "a".."e", 3.0..4.0, { k: "k"..."m" }, 5]))
p z[0], z[1], z[2].begin, z[2].end, z[3][:k], z[4]
p z[0].sum, z[1].to_a, z[1].begin.frozen?, z[3][:k].to_a
GC.start
p Marshal.load(Marshal.dump("aa".."ad")).to_a

# The Symbol name and a bound have distinct object records. The objects
# that follow them still use the correct link numbers.
names = [:a, :c, 1]
after = String.new("after the bounds")
linked = Marshal.load(Marshal.dump([:a.name, names[0]..names[1], after, after]))
p linked[0].equal?(linked[1].begin), linked[2].equal?(linked[3])
