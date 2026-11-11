# A receiverless call and a call on self split the same way.
class R < Array
  def add(x, kw) = push(x, **kw)
  def add2(x, kw) = self.push(x, **kw)
  def tot(kw) = sum(**kw)
  def tot2(kw) = self.sum(**kw)
  def pick(i, kw) = fetch(i, **kw)
end
r = R.new
p r.add(1, {}), r.add(2, {a: 1}), r.add2(3, {})
p r.class, r.size
q = R.new.push(1, 2)
p q.tot({}), q.tot2({})
p((q.tot({a: 1}) rescue $!.class))
p q.pick(0, {}), q.pick(9, {d: 1})
class Array
  def tot3(kw) = sum(**kw)
end
p [1, 2].tot3({})
