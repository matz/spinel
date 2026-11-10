# String#scrub! answers its receiver, and writes only a change: a frozen
# String with nothing to scrub is left alone and answered, while one with an
# invalid byte raises FrozenError. It had an emitter of its own that knew a
# variable, a shared handle's slot and a reader call, so a receiver that
# arrived another way -- the handle of a safe-navigation call's guard, as in
# `s.upcase!&.scrub!(f("?"))` -- went into a `const char *` and the C did not
# compile. It is now a row of the String bang family (the typed bangs'
# machinery, which `tr!` and `upcase!` use) that answers self and keeps an
# unchanged String the receiver's own, as `reverse!` already is.
def f(x) = x
def id(x) = x

# the value and the receiver, with and without a replacement
s = +"abc"
p s.scrub!, s
t = +"a\xffb"
r = t.scrub!
p r.bytes, t.bytes
u = +"a\xffb"
p u.scrub!("?"), u
v = +"a\xff\xfeb"
p v.scrub!("*"), v

# the safe-navigation tail and a nil receiver
s = +"abc"
p s.upcase!&.scrub!(f("?"))
p s.scrub!&.scrub!(f("?"))
t = +"a\xffb"
p t&.scrub!(f("?")), t
n = nil
p n&.scrub!
w = [+"x\xff", nil]
p w[0]&.scrub!("_"), w[1]&.scrub!("_")

# a receiver reached through a method, an ivar, a reader, an Array, a Hash
s = +"a\xffb"
p id(s).scrub!("!"), s
t = +"ok"
p id(t).scrub!.equal?(t)
@iv = +"q\xff"
p @iv.scrub!("-"), @iv
class Holder
  attr_reader :n
  def initialize(s) = @n = s
  def go = @n.scrub!("?")
  def go2 = @n&.scrub!("?")
end
k = Holder.new(+"z\xff")
p k.n.scrub!("+"), k.n
p Holder.new(+"g\xff").go, Holder.new(nil).go2
a = [+"m\xff"]
p a[0].scrub!("."), a
h = { k: +"n\xff" }
p h[:k].scrub!("#"), h
x = [1, +"a\xff"].last
p x.scrub!("?"), x
y = [1, "q"].last
p y.scrub!, y

# the receiver is the answer, and other names for it see the change
a = s = +"a\xffb"
s.scrub!("?")
p a, s.equal?(a)
b = [s]
s.scrub!("!")
p b
r = +"hello"
p r.scrub!.equal?(r)
r << " world"
p r
e = +"q\xff"
e.scrub!("1").concat("2")
p e

# frozen: raised only when the String would change
z = "abc".freeze
p((z.scrub! rescue $!.class))
z = "z\xff".freeze
p((z.scrub! rescue $!.class))
p((z.scrub!("?") rescue $!.class))
q = +"abc"
q.freeze
p((q.scrub!("?") rescue $!.class))
fz = [1, "q".freeze].last
p((fz.scrub! rescue $!.class))
nl = nil
p((nl.scrub! rescue $!.class))

# reverse!, the other self-answering bang
s = +"abcd"
p s.reverse!, s
t = +"xy"
p t.reverse!.reverse!, t
p id(t).reverse!
p t&.reverse!
p(("fz".freeze.reverse! rescue $!.class))
p [1, +"ab"].last.reverse!
p [1, 2].reverse!
