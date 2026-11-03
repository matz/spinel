# Time's copies of a Time subclass instance (getutc, getlocal) are fresh
# instances of its runtime class, a descendant's included, without its
# ivars; a frozen one raises FrozenError on utc or localtime unless the
# conversion changes nothing (utc on one in UTC); a reopen of Time that
# defines a copy's name answers with its own method.
class Stamp < Time
  attr_accessor :tag
end
class Sub2 < Stamp
  attr_accessor :x
end
def copy(st)
  st.getutc
end
a = Stamp.at(0)
a.tag = "a"
b = Sub2.at(0)
b.x = 7
ca = copy(a)
cb = copy(b)
p ca.class, ca.tag, cb.class, cb.x
p a.getlocal("+09:00").class, a.getlocal("+09:00").tag
items = [b, "x"]
p items[0].getutc.class

t = Stamp.at(0)
t.freeze
begin
  t.utc
rescue FrozenError => e
  p e.class
end
begin
  t.localtime("+01:00")
rescue FrozenError => e
  p e.class
end
u = Stamp.at(0).utc
u.freeze
p u.utc.equal?(u)
boxed = [Stamp.at(0), "x"]
boxed[0].freeze
begin
  boxed[0].utc
rescue FrozenError => e
  p e.class
end
zb = [Sub2.at(0), "x"]
p zb[0].getlocal("+09:00").class, zb[0].getlocal("+09:00").hour
z0 = zb[0]
p z0.localtime("+02:00").equal?(z0), z0.localtime("+02:00").class, zb[0].hour
zf = [Stamp.at(0), "x"]
zf[0].freeze
begin
  zf[0].localtime("+03:00")
rescue FrozenError => e
  p e.class
end
