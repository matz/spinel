# spinel: gc-minor
# String#insert, clear and slice! change the receiver's String in place,
# whichever way the receiver is reached: self and a bare call in a reopened
# String, a local, an ivar, a method's answer, an Array element and the
# handle a `&.` guard holds. Other names for the String see the change,
# insert and clear answer the receiver and slice! a new unfrozen String,
# and object_id holds across the changes. A frozen String raises
# FrozenError in the order CRuby checks: after the index for insert, before
# it for clear and slice!.

class String
  def wrap!
    self.insert(0, "<")
    self.insert(-1, ">")
  end

  def lead!(x)
    insert(0, x)
    self
  end

  def chop1!
    slice!(0)
  end

  def drop_s!(x)
    slice!(x)
    self
  end

  def strip_digits!
    r = self.slice!(/\d+/)
    [r, self]
  end

  def take2!
    self.slice!(0..1)
  end

  def wipe!
    clear
  end
end

s = +"abc"
t = s
id = s.object_id
p s.wrap!.equal?(s), t
p s.lead!("."), t
p s.chop1!, t
p (+"abcabc").drop_s!("ca")
p (+"ab12cd").strip_digits!
p (+"abcdef").take2!
p s.wipe!.equal?(s), t, s.object_id == id

# a local, and the values the four answer
u = +"hello"
v = u
r = u.insert(1, "-")
p r.equal?(u), v
x = u.slice!(0, 2)
p x, x.frozen?, u, v
r = u.clear
p r.equal?(u), v

# an ivar, a method's answer and an Array element
class Box
  def initialize = @s = +"inside"
  def s = @s
  def run
    @s.insert(-1, "!")
    [@s.slice!(1, 2), @s]
  end
end
b = Box.new
p b.run, b.s
b.s.insert(1, "+")
p b.s.slice!(-1), b.s
a = [+"one", +"two"]
a[1].insert(0, "<")
p a[1].slice!(/w/), a

# the handle a `&.` guard holds
def maybe(f) = f ? +"maybe" : nil
[true, false].each do |f|
  m = maybe(f)
  p m&.insert(0, "<")
  p m&.slice!(-1)
  p m&.slice!(0, 2)
  p m&.clear
  p m
end

# a key or a value that is the receiver itself
k = +"aba"
p k.slice!(k), k
k = +"abc"
k = +"q"
k.insert(0, k.dup); p k
k = +"ab"
k.insert(1, 2.9.to_s); p k
k = +"abcdef"
k.insert(2.9, "-"); p k

# frozen: the order of the checks
e = "".freeze
begin; e.clear; rescue FrozenError => x; p [x.class, x.message]; end
f = "abc".freeze
begin; p f.slice!(10); rescue FrozenError => x; p [x.class, x.message]; end
begin; p f.slice!("zz"); rescue FrozenError => x; p [x.class, x.message]; end
begin; p f.slice!(/q/); rescue FrozenError => x; p [x.class, x.message]; end
begin; f.insert(0, ""); rescue FrozenError => x; p [x.class, x.message]; end
begin; f.insert(9, "q"); rescue => x; p [x.class, x.message]; end
p f
