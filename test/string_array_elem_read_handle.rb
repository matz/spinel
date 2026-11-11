# spinel: gc-minor
# spinel: gc-stress
# A String Array holds each element as the String itself, and reading an
# element hands that String back, not a copy of its bytes: after each way
# of reading one (a[i], first, last, at, fetch, dig, pop, shift, delete_at,
# sample, min, max, and a block parameter bound to the element by each,
# each_with_index, map, select, find, for and the folds), a change made
# through the String that came out shows through every other holder, and
# the String that came out is the one object each time it is read.
#
# Sections: identity of each read; a block parameter's identity; nil
# elements; frozen literal elements; a String that left its Array (pop,
# shift, delete_at, an overwritten or cleared element) while another
# operand of the same expression allocates, which a collection under
# SPINEL_GC_STRESS must not free; evaluation order; a read taken as its
# bytes; delete and minmax; dig's one step and a parenthesised sequence
# ending in a read; every read inside a method; a read through `&.` (the
# element, its bytes, a nil receiver, an index that runs a block); a read
# held ahead of the call it is an argument of (eql?, casecmp?, equal?).
def make(n) = +"made#{n}"

def churn(n)
  x = +""
  n.times { |i| x = "g#{i}" * 3 }
  x
end

def pair(a, b) = [a, b]

# --- identity of each read -------------------------------------------------
a = [+"a", +"b", +"c"]
h0 = a[0]
a[0] << "1"
puts h0
a.first << "2"
puts h0
a.last << "3"
puts a[2]
a.at(1) << "4"
a.fetch(1) << "5"
a.dig(1) << "6"
a.min << "7"
a.max << "8"
puts a.inspect
puts a[0].equal?(a[0])
puts a.first.equal?(a[0])
puts a.last.equal?(a[-1])
puts a.at(0).equal?(h0)
puts a.fetch(0).equal?(h0)
puts a.dig(0).equal?(h0)
puts a.min.equal?(a[0])
puts a.max.equal?(a[2])
sm = [+"s1", +"s2"]
smp = sm.sample
puts sm.any? { |x| x.equal?(smp) }
smp << "S"
puts sm.any? { |x| x.end_with?("S") }
puts a[0].object_id == h0.object_id

t = a.pop
t << "x"
puts t
puts a.inspect
u = a.shift
u << "y"
puts u, h0.equal?(u)
puts a.inspect
b = [+"p", +"q", +"r"]
held = b[1]
d = b.delete_at(1)
puts d.equal?(held)
d << "!"
puts held
b.pop << "z"
puts b.inspect
c = [+"m", +"n"]
hn = c[1]
c.pop << "w"
puts hn
c.shift << "v"
puts c.inspect

# --- a block parameter is the element --------------------------------------
e = [+"x", +"y", +"z"]
he = e[1]
seen = []
e.each { |s| seen << s }
puts seen[1].equal?(he)
seen[0] << "0"
puts e.inspect
e.each { |s| s << "!" }
puts he
e.each_with_index { |s, i| s << i.to_s }
puts e.inspect
e.map { |s| s << "m" }
puts e.inspect
puts e.find { |s| s.equal?(he) }.equal?(he)
e.select { |s| s << "s" }
puts e.inspect
for s in e
  s << "f"
end
puts e.inspect
puts s.equal?(e[2])
e.each.with_index { |s2, i| s2 << "w#{i}" }
puts e.inspect
puts e.sort_by { |s3| s3 }.first.equal?(e.min)
puts e.map { |s4| s4.equal?(he) }.inspect
puts e.reverse_each.first.equal?(e.last)
e.each_slice(2) { |pr| pr.each { |s5| s5 << "." } }
puts e.inspect
e.each_cons(2) { |x1, x2| x2 << "," }
puts e.inspect
puts e.inject(+"") { |acc, s6| acc << s6[0] }
puts e.count { |s7| s7.equal?(he) }
puts e.any? { |s8| s8.equal?(he) }
puts e.index { |s9| s9.equal?(he) }
pr = proc { |s10| s10 }
puts pr.call(e[0]).equal?(e[0])
kept = nil
e.each { |s11| kept = s11 if s11.equal?(he) }
kept << "K"
puts he

# --- one-character and empty elements are one String each -----------------
oc = +"abc"
one = [oc[0], oc[1], +""]
one << oc[2]
one.push("abc"[0, 0])
ho = one[0]
puts one[0].equal?(one[0]), one.first.equal?(one[0]), one[0].equal?(ho)
one[0] << "!"
puts ho, one[0]
one.last << "?"
puts one.inspect
bo = one.pop
bo << "z"
puts bo, one.inspect
one.each { |x| x << "." }
puts one.inspect
chs = "xyz".chars
puts chs[0].equal?(chs[0])
chs[1] << "+"
chs.each { |c| c << "-" }
puts chs.inspect

# --- an element read is written into another Array as itself -----------------
src = [+"a", +"b", +"c"]
dst = []
dst << src[0]
dst.push(src.last)
dst[2] = src.at(1)
dst.insert(1, src.first)
lit = [src[0], src[1], src.fetch(2)]
puts dst[0].equal?(src[0]), dst[1].equal?(src[0]), dst[2].equal?(src[2]), dst[3].equal?(src[1])
puts lit[0].equal?(src[0]), lit[1].equal?(src[1]), lit[2].equal?(src[2])
dst[0] << "!"
puts src[0], lit[0]
moved = []
moved << src.pop
moved << src.shift
moved[0] << "?"
puts moved.inspect, src.inspect

# (...of sort_by, sum and map.with_index, whose blocks take it as the String)
bq = [+"x", +"y", +"z"]
bq.sort_by { |s11| s11 << "s"; s11 }
puts bq.sum("") { |s12| s12 << "+"; s12 }
bk = bq[0]
bq.map.with_index { |s13, i| puts s13.equal?(bk) if i == 0; s13 }
puts bq.inspect

# --- nil elements ----------------------------------------------------------
n = [+"x"]
n[2] = +"z"
p n[1]
p n.fetch(1)
p n.dig(1)
p n.at(1)
p n[1].nil?
p n.first
p n.last
n.each { |s| p s }
n.each_with_index { |s, i| p [s, i] }
p n.map { |s| s.nil? }
p n.compact
p n.pop
p n.pop
p n.shift
p n.shift
p n
p [].first
p [].last
p [].pop
p [].shift
p [].sample
p [+"q"][5]
nl = [+"a", nil]
nl.each { |s| puts s.nil? ? "nil" : s }
x = nl[1]
p x
p nl.delete_at(1)
p nl.delete_at(7)
p nl

# --- frozen literal elements -----------------------------------------------
lits = ["lit", "lit2"]
begin
  lits[0] << "x"
rescue FrozenError => ex
  puts ex.message
end
begin
  lits.first << "x"
rescue FrozenError => ex
  puts ex.message
end
begin
  lits.pop << "x"
rescue FrozenError => ex
  puts ex.message
end
begin
  lits.each { |s| s << "x" }
rescue FrozenError => ex
  puts ex.message
end
begin
  ["a"][0] << "x"
rescue FrozenError => ex
  puts ex.message
end
puts lits[0].equal?("lit")
puts lits.inspect

# --- a String out of its Array while another operand allocates --------------
keep = []
300.times do |i|
  g = [make(i), make(i + 1)]
  r = pair(g.pop, churn(40))
  keep << r[0] if i % 100 == 0
  g2 = [make(i), make(i + 1)]
  r2 = pair(g2.shift, churn(40))
  keep << r2[0] if i % 100 == 1
  g3 = [make(i), make(i + 1), make(i + 2)]
  r3 = pair(g3.delete_at(1), churn(40))
  keep << r3[0] if i % 100 == 2
  g4 = [make(i), make(i + 1)]
  r4 = [g4.pop, churn(40)]
  keep << r4[0] if i % 100 == 3
  g5 = [make(i), make(i + 1)]
  r5 = g5.pop + churn(40)
  keep << r5 if i % 100 == 4
  g6 = [make(i), make(i + 1)]
  r6 = "#{g6.shift}#{churn(40)}"
  keep << r6 if i % 100 == 5
  g7 = [make(i), make(i + 1)]
  s7 = g7[0]
  g7[0] = make(i + 2)
  r7 = pair(s7, churn(40))
  keep << r7[0] if i % 100 == 6
  g8 = [make(i), make(i + 1)]
  s8 = g8.first
  g8.clear
  r8 = pair(s8, churn(40))
  keep << r8[0] if i % 100 == 7
  g9 = [make(i), make(i + 1)]
  r9 = pair(g9.pop, g9.pop)
  churn(40)
  keep << r9[0] if i % 100 == 8
  g10 = [make(i), make(i + 1)]
  r10 = g10.pop.upcase + churn(40)
  keep << r10 if i % 100 == 9
  g11 = [make(i), make(i + 1)]
  r11 = pair(g11.last, (g11.pop; churn(40)))
  keep << r11[0] if i % 100 == 10
  g12 = [make(i), make(i + 1)]
  r12 = pair(g12.pop, (g12.clear; churn(40)))
  keep << r12[0] if i % 100 == 11
end
puts keep.inspect

# --- evaluation order -------------------------------------------------------
o = [+"a", +"b"]
r = pair(o[0], (o[0] = +"c"; 1))
puts r.inspect
r = pair(o[0], (o.clear; 1))
puts r.inspect
o = [+"a", +"b"]
r = pair(o.first, (o << +"z"; 1))
puts r.inspect
o = [+"a", +"b"]
r = pair(o.pop, (o.push(+"q"); 1))
puts r.inspect, o.inspect
o = [+"a", +"b"]
r = pair(o[1], (o = [+"other"]; 1))
puts r.inspect, o.inspect
o = [+"a", +"b"]
v = o[0]
r = pair(v, (o[0] << "!"; 1))
puts r.inspect
o = [+"a", +"b"]
r = [o[0], o[0] << "?"]
puts r.inspect

# --- a read through a face: the element's bytes -----------------------------
fa = [+"f0", +"f1", +"f2"]
fi = 1
puts "str=" + fa.fetch(fi)
puts fa.fetch(2)
puts "#{fa.fetch(0)}|#{fa.fetch(-1)}"
puts fa.fetch(fi).size, fa.fetch(fi).upcase, fa.fetch(fi) == "f1"
puts fa.at(fi) + fa.dig(0) + fa.min + fa.max + fa.first + fa.last + fa[fi]
begin
  puts fa.fetch(7)
rescue IndexError => err
  puts err.message
end
def fetch_text(a, i) = "got " + a.fetch(i)
puts fetch_text(fa, 0)
puts fa.inspect

# --- delete answers the element it removed ----------------------------------
da = [+"x", +"aa", +"y"]
dk = da[1]
dd = da.delete("aa")
puts dd.equal?(dk)
dd << "!"
puts dk, da.inspect
puts da.delete("zz").inspect
puts da.delete("x") + "?"
dn = +"y"
dy = da[0]
puts da.delete(dn).equal?(dy), da.inspect
dz = [+"n1", +"n2"]
dzk = dz[1]
dzn = ["n2", 1][0]
puts dz.delete(dzn).equal?(dzk), dz.delete(["zz", 1][0]).inspect, dz.inspect
def delete_in(a, s) = a.delete(s)
db = [+"p", +"q"]
dq = db[1]
delete_in(db, "q") << "#"
puts dq, db.inspect

# --- minmax answers the two elements ----------------------------------------
mm = [+"q", +"b", +"z"]
lo, hi = mm.minmax
lo << "<"
hi << ">"
puts mm.inspect
mp = mm.minmax
puts mp[0].equal?(mm[1]), mp[1].equal?(mm[2])
mm.clear
puts mm.minmax.inspect

# --- dig's one step and a parenthesised sequence are the element ------------
dg = [+"bb", +"aa"]
dg.dig(0).upcase!
puts dg.inspect
dg.dig(1).replace("r")
puts dg.inspect
(dg.size; dg[1]) << "!"
ps = (dg.size; dg[0])
ps << "?"
puts dg.inspect
fz = [+"z0", +"z1"]
fk = fz[1]
(fz.size; fz[1]).freeze
fz.dig(0).freeze
puts fk.frozen?, fz[0].frozen?
def dig_and_paren(a, k)
  puts((a.size; a[1]).equal?(k))
  puts a.dig(1).equal?(k)
  a.dig(0).upcase!
  a.dig(1).replace("in")
  (a.size; a[2]) << "!"
  puts k, a.inspect
end
dp = [+"m0", +"m1", +"m2"]
dig_and_paren(dp, dp[1])

# --- every read inside a method ---------------------------------------------
def reads_in_method(a)
  k0 = a[0]
  k1 = a[1]
  k2 = a[2]
  a[0] << "0"
  a.first << "f"
  a.last << "l"
  a.at(1) << "a"
  a.fetch(1) << "t"
  a.dig(1) << "d"
  a.min << "m"
  a.max << "x"
  puts a.inspect
  puts k0.equal?(a[0]), k0.equal?(a.first), k2.equal?(a.last), k1.equal?(a.at(1))
  puts k1.equal?(a.fetch(1)), k1.equal?(a.dig(1)), k0.equal?(a.min), k2.equal?(a.max)
  s = a.sample
  puts a.any? { |e| e.equal?(s) }
  puts a.fetch(1) + "|" + a.at(0) + "|" + a.dig(2) + "|" + a.min + "|" + a.max
  t = a.pop
  puts t.equal?(k2)
  t << "p"
  u = a.shift
  puts u.equal?(k0)
  u << "s"
  a << +"n1" << +"n2"
  kn = a[1]
  d = a.delete_at(1)
  puts d.equal?(kn)
  d << "D"
  e = a.delete("n2")
  e << "E"
  puts k0, k1, k2, kn, e, a.inspect
end
reads_in_method([+"ma", +"mb", +"mc"])

# --- a read through &. -------------------------------------------------------
def sn_last(xs) = xs&.last
def sn_first(xs) = xs&.first
def sn_idx(xs, i) = xs&.[](i)
def sn_bang(x) = x << "!"
def sn_at_sum(xs, ns) = xs&.[](ns.inject(0) { |s, n| s + n })
sa = [+"bb", +"aa", +"cc"]
puts sn_last(sa)
puts sn_last(nil).inspect, sn_first(nil).inspect, sn_idx(nil, 0).inspect
sn_bang(sa&.first)
sn_bang(sn_last(sa))
sx = sa&.[](1)
sx << "?"
puts sa.inspect
puts sa&.at(0), sa&.fetch(1), sa&.dig(2), sa&.min, sa&.max
puts (sa&.fetch(1)) + "+", "#{sa&.first}", (sa&.last).size
sk = sa[1]
puts (sa&.at(1)).equal?(sk), (sa&.fetch(1)).equal?(sk), (sa&.dig(1)).equal?(sk), (sa&.[](1)).equal?(sk)
puts (sa&.min).equal?(sk), (sa&.max).equal?(sa[2]), sn_idx(sa, 1).equal?(sk), sn_first(sa).equal?(sa[0])
ss = sa&.sample
puts sa.any? { |e| e.equal?(ss) }
kp = sa[2]
sp = sa&.pop
puts sp.equal?(kp)
sp << "p"
ks = sa[0]
su = sa&.shift
puts su.equal?(ks)
su << "s"
puts kp, ks, sa.inspect
sb = [+"d0", +"d1", +"d2"]
kd = sb[1]
sd = sb&.delete_at(1)
puts sd.equal?(kd)
sd << "D"
ke = sb[0]
puts (sb&.delete("d0")).equal?(ke), kd, sb.inspect
# (an index that runs a block: the guard fills a slot)
ns = [1, 1]
sc = [+"h0", +"h1", +"h2"]
puts sn_at_sum(sc, ns)
puts sn_at_sum(nil, ns).inspect
sn_bang(sc&.[](ns.inject(0) { |s, n| s + n }))
def sn_push(xs)
  b = []
  b << xs&.fetch(0)
  b[0] << "^"
  b
end
puts sn_push(sc).inspect
sy = sc&.fetch(ns.map { |n| n - 1 }.sum)
sy << "#"
sz = nil
sz = sc if ns.size == 2
sw = sz&.[](ns.inject(0) { |s, n| s + n } - 1)
sw << "w"
puts sz&.last, sc.inspect

# --- an element read held ahead of its call ---------------------------------
hel = [+"a", +"b", +"ae"]
hx = +"a"
puts hx.eql?((hx << "e"; hel[2]))
puts hx
hx = +"a"
puts hx.casecmp?((hx << "E"; hel[2]))
hx = +"a"
puts hx.equal?(hel[(hx << "e").size]), hel[2].equal?(hel[(hx << "e").size - 1]), hx
hx = +"a"
puts hx.eql?((hx << "e"; hel.last)), hx.eql?((hx.upcase!; hel.first)), hx.casecmp?((hx << "f"; hel.min))
hx = +"b"
puts hx.eql?((hx << "!"; hel.pop)), hx.casecmp?((hx.chop!; hel.pop)), hel.inspect
def held_same(o, x, k) = k.equal?((x << "e"; o[1]))
hk = [+"k0", +"k1"]
puts held_same(hk, +"q", hk[1]), held_same(hk, +"q", hk[0])

# --- a removed element read as bytes, and a needle that is no String ---------
bx = [+"a", +"b", +"c"]
puts bx.slice!(1) + "x", bx.inspect
dn = [+"a", +"b"]
dr = dn.delete(1)
dr << "x" if dr
p dr, dn

# --- a read in parentheses bound ahead of a call on a receiver that may be nil
pn = [+"a", +"b", +"c", +"d", +"e", +"f"]
pw = nil
pw = [+"k"] if pn.size > 0
pw.concat([(pn.pop)])
puts pw.inspect, pn.inspect
pv = nil
pv = [+"k"] if pn.size > 0
pv.push((pn.pop))
pv.unshift((pn.shift))
puts pv.inspect, pn.inspect
pv[1] << "!"
pu = nil
pu = +"q" if pn.size > 0
pu.concat((pn.pop))
puts pu, pn.inspect
