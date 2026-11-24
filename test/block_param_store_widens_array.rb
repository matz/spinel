# A block parameter that stores another kind into the Array a yield or a
# block's call binds it to widens that Array where it is built, whichever
# parameter binds it (a required, an optional, a post, a keyword) and
# however many methods hand the block on.

def y1(a) = yield(a)
def y2(a, b) = yield(a, b)
def yk(a) = yield(k: a)
def call1(a, &b) = b.call(a)
def fwd1(a, &b) = call1(a, &b)
def fwd2(a, &b) = fwd1(a, &b)

a = [+"s"]; y1(a) { |x| x << 1 }; p a
a = [+"s"]; y1(a) { |x| x.push(2.5) }; p a
a = [1]; y1(a) { |x| x.concat(["t"]) }; p a
a = [1]; y1(a) { |x| x.insert(0, :q) }; p a
a = [1]; y1(a) { |x| x[1] = "u" }; p a
a = [1.5]; y1(a) { |x| x.unshift(nil) }; p a
a = [+"s"]; y1(a) { |x = nil| x << 3 }; p a
a = [+"s"]; y2(0, a) { |i, x = nil| x << i }; p a
a = [+"s"]; y2(0, a) { |i, *r, x| x << 4 }; p a
a = [+"s"]; yk(a) { |k:| k << 5 }; p a
a = [+"s"]; y1(a) { |x, k: 6| x << k }; p a
a = [+"s"]; fwd2(a) { |x| x << 7 }; p a
a = [+"s"]; call1(a) { |x| x << 8; p x.last + 1; p x[0].upcase }; p a

# one Array reaching the block twice, and a value that may be nil
b = [+"t"]
y1(b) { |x| x << 9 }
y1(b) { |x| x << :z }
p b
c = b.size > 2 ? [+"n"] : nil
y1(c) { |x| x << 10 if x }
p c

# a Hash the block stores another key into
def yh(h) = yield(h)
h = { a: 1 }
yh(h) { |t| t[:k] = "x" }
p h

class P1; def run(a) = yield(a); end
class S1 < P1; def run(a, &b) = super(a, &b); end
class S4 < P1; def run(a, &b) = super; end
a = [+"s"]; S1.new.run(a) { |x| x << 11 }; p a
a = [+"s"]; S4.new.run(a) { |x| x << 12 }; p a

# a keyword a `**` operand or a computed key supplies
def ykw(**h) = yield(**h)
b = [+"s"]; o = { k: b }; ykw(**o) { |k: nil| k << 13 }; p b
pk = proc { |x, k: nil| k << 14 }
b = [+"s"]; o = { k: b }; pk.call(1, **o, **{}); p b
c = [+"n"]; nm = :k; pk.call(1, nm => c); p c
pz = proc { |x, k: 0| x << k }
d = [+"d"]; pz.call(d, **{}); p d
