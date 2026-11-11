# spinel: gc-minor
# String#[]=, insert, clear and slice! change the receiver's String in place,
# whichever way the receiver is reached: self and a bare call in a reopened
# String, a local, an ivar, a method's answer, an Array element and the
# handle a `&.` guard holds. Other names for the String see the change,
# insert and clear answer the receiver, []= answers the right-hand object
# and slice! a new unfrozen String, and object_id holds across the changes.
# A frozen String raises FrozenError in the order CRuby checks: after the
# index for []= and insert, before it for clear and slice!.

class String
  def cap0!
    self[0] = self[0].upcase
    self
  end

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

  def mask!
    self[1..-2] = "*" * (length - 2) if length > 2
    self
  end

  def edit!
    self[0, 1] = "E"
    s2 = slice!(-1)
    insert(1, s2)
    self
  end

  def wipe!
    clear
  end
end

s = +"abc"
t = s
id = s.object_id
p s.cap0!.equal?(s), t
p s.wrap!.equal?(s), t
p s.lead!("."), t
p s.chop1!, t
p (+"abcabc").drop_s!("ca")
p (+"ab12cd").strip_digits!
p (+"abcdef").take2!
p (+"secret").mask!
p (+"word").edit!
p s.wipe!.equal?(s), t, s.object_id == id

# a local, and the values the four answer
u = +"hello"
v = u
r = (u[0] = "J")
p r, u, v
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
    @s[0] = "I"
    @s.insert(-1, "!")
    [@s.slice!(1, 2), @s]
  end
end
b = Box.new
p b.run, b.s
b.s[0] = "Z"
b.s.insert(1, "+")
p b.s.slice!(-1), b.s
a = [+"one", +"two"]
a[0][0] = "O"
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
k[k] = "x"; p k
k = +"q"
k.insert(0, k.dup); p k
k = +"ab"
k.insert(1, 2.9.to_s); p k
k = +"abcdef"
k.insert(2.9, "-"); p k

# a regexp key with a group
g = +"hello world"
g[/(o) (w)/, 2] = "W"; p g
g[/(l+)/, -1] = "L"; p g
begin; g[/xyz/] = "q"; rescue IndexError => e; p [e.class, e.message]; end
begin; g[/(h)/, 3] = "q"; rescue IndexError => e; p [e.class, e.message]; end
begin; g[/(h)/, -3] = "q"; rescue IndexError => e; p [e.class, e.message]; end
begin; g[/(h)|(z)/, 2] = "q"; rescue IndexError => e; p [e.class, e.message]; end
p g

# frozen: the order of the checks
e = "".freeze
begin; e.clear; rescue FrozenError => x; p [x.class, x.message]; end
f = "abc".freeze
begin; p f.slice!(10); rescue FrozenError => x; p [x.class, x.message]; end
begin; p f.slice!("zz"); rescue FrozenError => x; p [x.class, x.message]; end
begin; p f.slice!(/q/); rescue FrozenError => x; p [x.class, x.message]; end
begin; f.insert(0, ""); rescue FrozenError => x; p [x.class, x.message]; end
begin; f.insert(9, "q"); rescue => x; p [x.class, x.message]; end
begin; f[0, 0] = ""; rescue FrozenError => x; p [x.class, x.message]; end
begin; f[9] = "q"; rescue => x; p [x.class, x.message]; end
begin; f[0] = 5; rescue => x; p [x.class, x.message]; end
p f

# the String takes the encoding the change leaves it in: an ASCII-only
# binary String given a UTF-8 text becomes UTF-8, and clear keeps it
def enc(s) = [s, s.length, s.encoding.to_s]
p enc("a".b.insert(1, "é"))
p enc("é".b.insert(0, "a"))
p enc((+"a").insert(1, "x".b))
p enc("a".b.clear)

# s[/re/, n] = v reaches every group of the pattern, past the 16th too
re20 = /(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)(m)(n)(o)(p)(q)(r)(s)(t)/
[16, 17, 20, -1, 21].each do |n|
  s = +"abcdefghijklmnopqrst"
  begin; s[re20, n] = "X"; p s; rescue IndexError => x; p [x.class, x.message]; end
end

# []= leaves the String in the encoding the splice has: an ASCII-only
# binary String given a UTF-8 text becomes UTF-8, for every key form, typed
# or boxed; an empty or binary text keeps it binary
def bx(v) = [v, 1][0]
def encs(s) = p([s.length, s.encoding.to_s, s.bytesize])
s = "a".b; s["a"] = "é"; encs(s)
s = "a".b; s[/a/] = "é"; encs(s)
s = "a".b; s[/(a)/, 1] = "é"; encs(s)
s = "a".b; s[0] = "é"; encs(s)
s = "a".b; s[0, 1] = "é"; encs(s)
s = "a".b; s[0..0] = "é"; encs(s)
s = "a".b; s[bx("a")] = "é"; encs(s)
s = "a".b; s[bx(/a/)] = "é"; encs(s)
s = "a".b; s[bx(0)] = "é"; encs(s)
s = "a".b; s[bx(0..0)] = "é"; encs(s)
s = "a".b; s[bx(0), 1] = "é"; encs(s)
s = "a".b; s[0.0] = "é"; encs(s)
s = "ab".b; s[/a/] = ""; encs(s)
s = "ab".b; s[bx(/a/)] = ""; encs(s)
s = +"ab"; s[0] = "\xff".b; encs(s)
