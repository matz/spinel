# spinel: gc-minor
# spinel: gc-stress
# A String Array stores the String it is given, not a copy of it: after
# each way of writing an element (<<, push, []= as a statement and as a
# value, insert, unshift, fill, concat and the Array literal), the element
# is the object written, so a change made to it through either name shows
# through the other. Each form is tried with a String held in a variable,
# a method's answer, a builtin's answer, a frozen literal, a one-character
# String and nil, and with the Array's edge cases: a frozen Array, nil
# elements, negative and past-the-end indexes, fill with a block, concat
# with itself and a pushed value that changes the Array it goes into.
# Everything above the last section guards what already held.
#
# The last section ("a String whose bytes are static") is the fix: a
# one-character or empty String computed in place (s[0], s[0, 0], "ab" * 0)
# has static bytes, and the store used to make a new String of them at
# each index fill wrote and for the value []= answered. fill now stores
# one object at every index and []= answers the String it stored. The
# store's new String was also made while a receiver a method had just
# answered was held by no root, so under SPINEL_GC_STRESS=2 the last
# loop stopped with the mark on a freed slot; the receiver is rooted now.
def make(n) = +"made#{n}"

def check(label, a, i, v)
  same = v.nil? ? a[i].nil? : a[i].equal?(v)
  puts "#{label}: #{same} #{a.inspect}"
end

s = +"abc"

vh = s
ah = [+"x"]
ah << vh
check("<< handle", ah, 1, vh)
ah.push(vh, vh)
check("push handle", ah, 3, vh)
ah[0] = vh
check("[]= handle", ah, 0, vh)
rh = (ah[1] = vh)
check("[]= value handle", ah, 1, vh)
puts "  answer same: #{rh.equal?(vh)}"
ah.insert(1, vh)
check("insert handle", ah, 1, vh)
ah.unshift(vh)
check("unshift handle", ah, 0, vh)
ah.fill(vh, 1, 2)
check("fill handle", ah, 2, vh)
bh = [+"y"]
bh.concat(ah)
check("concat handle", bh, 1, vh)
lh = [+"z", vh]
check("literal handle", lh, 1, vh)

vm = make(1)
am = [+"x"]
am << vm
check("<< method", am, 1, vm)
am.push(vm, vm)
check("push method", am, 3, vm)
am[0] = vm
check("[]= method", am, 0, vm)
rm = (am[1] = vm)
check("[]= value method", am, 1, vm)
puts "  answer same: #{rm.equal?(vm)}"
am.insert(1, vm)
check("insert method", am, 1, vm)
am.unshift(vm)
check("unshift method", am, 0, vm)
am.fill(vm, 1, 2)
check("fill method", am, 2, vm)
bm = [+"y"]
bm.concat(am)
check("concat method", bm, 1, vm)
lm = [+"z", vm]
check("literal method", lm, 1, vm)

vb = s.upcase
ab = [+"x"]
ab << vb
check("<< builtin", ab, 1, vb)
ab.push(vb, vb)
check("push builtin", ab, 3, vb)
ab[0] = vb
check("[]= builtin", ab, 0, vb)
rb = (ab[1] = vb)
check("[]= value builtin", ab, 1, vb)
puts "  answer same: #{rb.equal?(vb)}"
ab.insert(1, vb)
check("insert builtin", ab, 1, vb)
ab.unshift(vb)
check("unshift builtin", ab, 0, vb)
ab.fill(vb, 1, 2)
check("fill builtin", ab, 2, vb)
bb = [+"y"]
bb.concat(ab)
check("concat builtin", bb, 1, vb)
lb = [+"z", vb]
check("literal builtin", lb, 1, vb)

vl = "lit"
al = [+"x"]
al << vl
check("<< literal", al, 1, vl)
al.push(vl, vl)
check("push literal", al, 3, vl)
al[0] = vl
check("[]= literal", al, 0, vl)
rl = (al[1] = vl)
check("[]= value literal", al, 1, vl)
puts "  answer same: #{rl.equal?(vl)}"
al.insert(1, vl)
check("insert literal", al, 1, vl)
al.unshift(vl)
check("unshift literal", al, 0, vl)
al.fill(vl, 1, 2)
check("fill literal", al, 2, vl)
bl = [+"y"]
bl.concat(al)
check("concat literal", bl, 1, vl)
ll = [+"z", vl]
check("literal literal", ll, 1, vl)

vc = s[0]
ac = [+"x"]
ac << vc
check("<< onechar", ac, 1, vc)
ac.push(vc, vc)
check("push onechar", ac, 3, vc)
ac[0] = vc
check("[]= onechar", ac, 0, vc)
rc = (ac[1] = vc)
check("[]= value onechar", ac, 1, vc)
puts "  answer same: #{rc.equal?(vc)}"
ac.insert(1, vc)
check("insert onechar", ac, 1, vc)
ac.unshift(vc)
check("unshift onechar", ac, 0, vc)
ac.fill(vc, 1, 2)
check("fill onechar", ac, 2, vc)
bc = [+"y"]
bc.concat(ac)
check("concat onechar", bc, 1, vc)
lc = [+"z", vc]
check("literal onechar", lc, 1, vc)

vn = nil
an = [+"x"]
an << vn
check("<< nil", an, 1, vn)
an.push(vn, vn)
check("push nil", an, 3, vn)
an[0] = vn
check("[]= nil", an, 0, vn)
rn = (an[1] = vn)
check("[]= value nil", an, 1, vn)
puts "  answer same: #{rn.nil?}"
an.insert(1, vn)
check("insert nil", an, 1, vn)
an.unshift(vn)
check("unshift nil", an, 0, vn)
an.fill(vn, 1, 2)
check("fill nil", an, 2, vn)
bn = [+"y"]
bn.concat(an)
check("concat nil", bn, 1, vn)
ln = [+"z", vn]
check("literal nil", ln, 1, vn)

# values written as they are computed, with no variable to hold them: each
# element is a String of its own, so changing one leaves the others
d = [+"d"]
d << s.upcase
d.push(s[1], make(3), "lit")
d[0] = s.downcase
d.insert(1, s[2])
d.unshift(make(4))
d.concat([s.upcase, s[0]])
d2 = [s.upcase, s[0], "lit", make(5), nil]
d[1] << "?"
d2[0] << "!"
p d, d2, s
# a change made through the element shows through the variable, and back
t = +"mut"
x1 = []
x1 << t
x1[0] << "!"
puts t
t << "?"
puts x1[0]
x1[1] = t
x1[1].upcase!
p x1, t

# a method's answer stored once is one object wherever it went
m = make(2)
x2 = [m]
x2.push(m)
x2.insert(0, m)
p x2.map { |e| e.equal?(m) }

# indexes: negative, past the end (the gap is nil)
x3 = [+"p", +"q"]
x3[-1] = s
x3[4] = s
p x3, x3[-1].equal?(s), x3[4].equal?(s)
x3.insert(-2, s)
p x3
x3.fill { |i| "f#{i}" }
p x3

# concat with itself copies the elements it had
x4 = [s, +"w"]
x4.concat(x4)
p x4, x4[2].equal?(s)

# a pushed value that changes the Array it goes into
x5 = [+"g"]
x5.push(x5.pop + "h")
p x5
x5 << (x5[0] = +"k")
p x5, x5[0].equal?(x5[1])

# a frozen Array refuses every write
f = [+"c"].freeze
[-> { f << s }, -> { f[0] = s }, -> { f.insert(0, s) }, -> { f.unshift(s) },
 -> { f.fill(s) }, -> { f.concat([s]) }].each do |w|
  begin
    w.call
  rescue FrozenError => e
    puts e.class
  end
end
p f

# a receiver or an index that allocates runs before the value is stored,
# and the String written survives what it allocated
def idx(i) = ("x" * 64).size - 64 + i
$arrs = [[+"r0"], [+"r1"]]
def arr(i) = $arrs[i + ("y" * 32).size - 32]
c = +"chars"
x8 = [+"a0", +"a1", +"a2"]
40.times do |i|
  x8[idx(i % 3)] = c[i % 5]
  arr(i % 2) << c[(i + 1) % 5]
  arr(0).push(c[i % 5], "lit")
end
p x8, $arrs[0].size, $arrs[1].size, $arrs[0].last(3), $arrs[1].last(2)

# a String whose bytes are static, computed where it is written: fill
# stores that one String at each index, and []= answers the String stored
cs = +"abc"
z = [+"x", +"y", +"z"]
z.fill(cs[0])
p z[0].equal?(z[1]), z[1].equal?(z[2])
z[0] << "!"
p z
z.fill(cs[0, 0])
p z[0].equal?(z[2])
z.fill("ab" * 0)
p z[0].equal?(z[1])
w = [+"p", +"q"]
y = (w[1] = cs[1])
p y.equal?(w[1])
y << "?"
p w, y
y = (w[0] = cs[0, 0])
p y.equal?(w[0])
y = (w[0] = "ab" * 0)
p y.equal?(w[0])

# a fresh receiver from a method, stored into with such a String: the
# store must not collect the receiver it is writing into
def mk = [+"m"]
300.times { |i| mk[i % 2] = cs[i % 3] }
puts "done"
