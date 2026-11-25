# A frozen Array raises FrozenError from push, unshift and insert even when
# its arguments store nothing: a splat with no elements and an empty `**h`,
# on a boxed receiver as on a typed one. An unfrozen one answers itself.

def t(label)
  r = yield
  p [label, r]
rescue => e
  p [label, e.class]
end

a = [[1], +"s"][0]
a.freeze
h = {}; g = {}; e = []
def l(*x) = x

t(:unshift_kw) { a.unshift(*e, **h) }
t(:push_kw) { a.push(*e, **h) }
t(:unshift_call_kw) { a.unshift(*l, **h) }
t(:unshift_two_kw) { a.unshift(**h, **g) }
t(:insert_kw) { a.insert(0, *e, **h) }
def u2(x, *r, **k) = x.unshift(*r, **k)
def p2(x, *r, **k) = x.push(*r, **k)
t(:unshift_rest) { u2(a) }
t(:push_rest) { p2(a) }
t(:unshift_splat) { a.unshift(*e) }
t(:push_splat) { a.push(*e) }
t(:push_none) { a.push }
t(:two) { [a.push(*e), a.unshift(*e)] }
k = {z: 1}
t(:push_kw_full) { a.push(*e, **k) }

# typed receivers, as statements
c = [1, 2].freeze
begin; c.unshift(*e, **h); p :no_error; rescue => x; p x.class; end
begin; c.push(*e, **h); p :no_error; rescue => x; p x.class; end
d = [1, "x"].freeze
begin; d.unshift(*e, **h); p :no_error; rescue => x; p x.class; end
begin; d.push(*e, **h); p :no_error; rescue => x; p x.class; end

b = [[1], +"s"][0]
p b.unshift(*e, **h), u2(b), p2(b, 2), b.push(*e)

# the values a splat brings go in order and are taken before the first goes
# in, the receiver's own too
q = [[1], +"s"][0]
m = [2, 3]; kz = {z: 1}
p q.unshift(*m, **kz), q.push(5, *m, 6).size
w = [[9, 8], +"s"][0]
p w.unshift(*w), w.push(*w)

# a splat into a boxed receiver's insert goes to the one many-value entry:
# a String of either face takes exactly one value, and a receiver with no
# insert raises even when the splat spreads nothing
def ip(a, i, *r) = a.insert(i, *r)
def tr(l)
  r = yield
  p [l, r]
rescue => e
  p [l, e.class]
end
sb = +"ab"; sb << "cd"
tr(:strbuf) { ip(sb, 1, "z") }
tr(:strbuf2) { ip(sb, 1, "z", "y") }
tr(:frozen_s) { ip("ab", 1, "z") }
tr(:frozen_a) { ip([3].freeze, 0, 1) }
tr(:int0) { ip(5, 0) }
tr(:nil0) { ip(nil, 0) }
tr(:neg2) { ip([1, 2, 3], -2, 8, 9) }
tr(:negsmall) { ip([1, 2], -5, 8, 9) }
