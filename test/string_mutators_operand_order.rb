# spinel: gc-minor
# String#[]=, insert and slice! run every operand once, in Ruby's order --
# the receiver, the keys, the value -- before any check the method makes,
# and then check and convert them in CRuby's order. Any operand type is
# taken: a Float index truncates, nil, a String or a Range as an index
# raise CRuby's TypeError. `x[k] = v` answers v itself. A bang's answer on a
# held chain (a Hash value, an Array element passed through a call) is
# changed through its handle, so a value that grows the String is kept.

def t
  r = yield
  p r
rescue => e
  p [e.class, e.message]
end

# the value runs once, before the key is matched
class W; def to_str = "Q"; end
$n = 0
def mk = ($n += 1; W.new)
def five; puts "five ran"; 5; end
s = +"abc"
s["b"] = mk
p s, $n
u = +"abc"
u[/(b)/, 1] = mk
p u, $n
t { s["zz"] = five }
t { s[/(z)/, 1] = five }
def mk2(s); s.replace(+"zzb"); W.new; end
s = +"abc"
s["b"] = mk2(s)
p s

# the answer is the right-hand object
w = W.new
r1 = (s[0] = w)
p r1.class, r1.equal?(w), s
v = [W.new, 1][0]
k = [0, "x"][0]
r2 = (s[k] = v)
p r2.class, s
r3 = (s[1, 1] = "yz")
p r3, s

# every operand type of the two-operand forms
t { (+"abc").slice!(0, nil) }
t { (+"abc").slice!(0, 1.5) }
t { (+"abc").slice!(1.5, 1) }
t { (+"abc").slice!(0, "1") }
t { (+"abc").slice!(nil, 1) }
t { (+"abc").slice!("b", 1) }
t { (+"abc").slice!(0..1, 1) }
t { "abc".freeze.slice!(0, nil) }
t { x = +"abc"; x[0, nil] = "x"; x }
t { x = +"abc"; x[0, 1.5] = "x"; x }
t { x = +"abc"; x[1.5, 1] = "x"; x }
t { x = +"abc"; x[1.7] = "X"; x }
t { x = +"abc"; x[nil] = "x"; x }
t { x = +"abc"; x[:a] = "x"; x }
t { x = +"abc"; x[0..1, 1] = "x"; x }
t { x = +"abcdef"; [x.slice!(0.5), x.slice!(1.2, 2.8), x] }
t { x = +"abc"; x.insert(1.9, "-") }

# CRuby's order of the checks: frozen, out of range and a value that is no
# String, for each kind of key
f = "abc".freeze
t { f[10] = 5 }
t { f[10] = "x" }
t { f[1] = 5 }
t { f[10, 1] = 5 }
t { f[10, 1] = "x" }
t { f[10..12] = 5 }
t { f[10..12] = "x" }
t { f[1..2] = 5 }
t { f["z"] = 5 }
t { f["b"] = 5 }
t { f[/z/] = 5 }
t { f[/b/] = 5 }
t { f[/(b)/, 2] = 5 }
t { f.insert(10, 5) }
t { f.insert(10, "x") }
t { f.insert(1, 5) }
t { f.slice!(10) }
t { f.slice!(nil) }
t { f.slice!("z") }
t { f.slice!(/z/) }
m = +"abc"
t { m[10..12] = 5 }
t { m[-10..1] = nil }
t { m[4..] = "E" }
t { m[1..2] = 5 }
t { m[10] = "x" }
t { m[10, -1] = "x" }
p m

# a bang's answer on a held chain takes the change, also when it grows
def bid(x) = x
bid(1)
h = {k: +" ab "}
h[:k].strip!.insert(0, "-" * 300)
p h[:k].length
h[:k].upcase![0, 1] = "Q" * 300
p h[:k].length
p h[:k].downcase!.slice!(0, 299).length, h[:k].length
h[:k].upcase!.clear
p h[:k]
s = +"ab"
a = [s, 1]
bid(a[0]).upcase!.insert(0, "<" * 100)
p s.length
bid(a[0]).downcase![0, 1] = "Q" * 200
p s.length
p bid(a[0]).upcase!.slice!(0, 250).length, s
p(bid(a[0]).downcase!&.insert(0, "x"), s)

# a receiver that is an expression -- a sequence, a call, a conditional, a
# begin block -- is changed through the handle it yields, and a mutator
# that is a block's last statement answers the block's value
def nf = nil
def mq = +"qrs"
$c = true
z = +"Z"
t { (nf; +"qrs")[2] = z }
t { mq[0] = z }
t { ($c ? +"qrs" : +"abc")[1] = z }
t { (begin; +"qrs"; end)[0, 2] = z }
t { (nf; +"qrs")[/r/] = z }
t { (nf; +"qrs")["s"] = z }
t { (nf; +"qrs")[0..1] = z }
t { (nf; +"qrs").insert(1, z) }
t { ($c ? +"qrs" : +"abc").insert(0, z) }
t { (begin; +"qrs"; end).insert(3, z) }
t { (nf; +"qrs").clear }
t { mq.clear }
t { (begin; +"qrs"; end).clear }
t { (nf; +"qrs").slice!(1) }
t { mq.slice!(0, 2) }
t { ($c ? +"qrs" : +"abc").slice!(/r/) }
t { (begin; +"qrs"; end).slice!(1..) }
t { (nf; "qrs".freeze)[1] = z }
t { (nf; "qrs".freeze).insert(5, z) }
t { (nf; "qrs".freeze).slice!(5) }
t { (nf; "qrs".freeze).clear }
x = +"abc"
t { (nf; x)[0] = z; x }
t { (nf; x).insert(0, z); x }
t { ($c ? x : nil).slice!(0); x }
t { (begin; x; end).clear; x }

# `x[start, len] = v`: CRuby converts the start and the length, raises for a
# negative length, converts the value, checks the start and then the frozen
# state; every pair of two faults, and the three of a frozen String
t { x = +"ab"; x.freeze; x[1, 1] = "x"; x }   # F
t { x = +"ab"; x[1, -1] = "x"; x }   # L
t { x = +"ab"; x[10, 1] = "x"; x }   # S
t { x = +"ab"; x[1, 1] = 5; x }   # V
t { x = +"ab"; x[nil, 1] = "x"; x }   # Ns
t { x = +"ab"; x[1, "1"] = "x"; x }   # Nl
t { x = +"ab"; x.freeze; x[1, -1] = "x"; x }   # F+L
t { x = +"ab"; x.freeze; x[10, 1] = "x"; x }   # F+S
t { x = +"ab"; x.freeze; x[1, 1] = 5; x }   # F+V
t { x = +"ab"; x.freeze; x[nil, 1] = "x"; x }   # F+Ns
t { x = +"ab"; x.freeze; x[1, "1"] = "x"; x }   # F+Nl
t { x = +"ab"; x[10, -1] = "x"; x }   # L+S
t { x = +"ab"; x[1, -1] = 5; x }   # L+V
t { x = +"ab"; x[nil, -1] = "x"; x }   # L+Ns
t { x = +"ab"; x[10, 1] = 5; x }   # S+V
t { x = +"ab"; x[10, "1"] = "x"; x }   # S+Nl
t { x = +"ab"; x[nil, 1] = 5; x }   # V+Ns
t { x = +"ab"; x[1, "1"] = 5; x }   # V+Nl
t { x = +"ab"; x[nil, "1"] = "x"; x }   # Ns+Nl
t { x = +"ab"; x.freeze; x[0, -1] = 5; x }   # F+L+V
t { x = +"ab"; x.freeze; x[5, -1] = 5; x }   # F+L+S+V
t { x = +"ab"; x[0, -1] = 5; x }   # L+V

# a right-hand object with #to_str read from a global, a class variable or
# a constant: the conversion reads the object the row holds
class TS; def initialize(n) = (@n = n); attr_reader :n; def to_str = "t#{@n}"; end
$tsv = TS.new(1)
class TSH; @@v = TS.new(2); def self.run(s) = (r = (s[0] = @@v); [r.n, s]); end
TSC = TS.new(3)
s = +"abc"
r = (s[0] = $tsv)
p r.n, s
p TSH.run(+"abc")
r = (s[1] = TSC)
p r.n, s
r = s.insert(0, $tsv)
p r
