# spinel: gc-minor
# String#insert runs every operand once, in Ruby's order -- the receiver,
# the index, the value -- before any check the method makes, and then
# checks and converts them in CRuby's order. A Float index truncates. A
# bang's answer on a held chain (a Hash value, an Array element passed
# through a call) is changed through its handle, so a value that grows the
# String is kept.

def t
  r = yield
  p r
rescue => e
  p [e.class, e.message]
end

# every operand type of the two-operand forms
t { x = +"abc"; x.insert(1.9, "-") }

# CRuby's order of the checks: frozen, out of range and a value that is no
# String, for each kind of key
f = "abc".freeze
t { f.insert(10, 5) }
t { f.insert(10, "x") }
t { f.insert(1, 5) }

# a bang's answer on a held chain takes the change, also when it grows
def bid(x) = x
bid(1)
h = {k: +" ab "}
h[:k].strip!.insert(0, "-" * 300)
p h[:k].length
h[:k].upcase!.clear
p h[:k]
s = +"ab"
a = [s, 1]
bid(a[0]).upcase!.insert(0, "<" * 100)
p s.length
p(bid(a[0]).downcase!&.insert(0, "x"), s)

# a receiver that is an expression -- a sequence, a call, a conditional, a
# begin block -- is changed through the handle it yields, and a mutator
# that is a block's last statement answers the block's value
def nf = nil
def mq = +"qrs"
$c = true
z = +"Z"
t { (nf; +"qrs").insert(1, z) }
t { ($c ? +"qrs" : +"abc").insert(0, z) }
t { (begin; +"qrs"; end).insert(3, z) }
t { (nf; +"qrs").clear }
t { mq.clear }
t { (begin; +"qrs"; end).clear }
t { (nf; "qrs".freeze).insert(5, z) }
t { (nf; "qrs".freeze).clear }
x = +"abc"
t { (nf; x).insert(0, z); x }
t { (begin; x; end).clear; x }

# a right-hand object with #to_str read from a global, a class variable or
# a constant: the conversion reads the object the row holds
class TS; def initialize(n) = (@n = n); attr_reader :n; def to_str = "t#{@n}"; end
$tsv = TS.new(1)
TSC = TS.new(3)
s = +"abc"
r = s.insert(0, $tsv)
p r
