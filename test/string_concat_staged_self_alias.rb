# frozen_string_literal: true
# A multi-argument concat takes every argument before it appends the first,
# as CRuby does, whichever String an argument is: the receiver itself, a
# view of it, a frozen literal, a built String or a nil-guarded value. The
# arguments are staged with no proof that any of them is a new String.

s = +"ab"
r = s.concat(s, s)
puts s, r.equal?(s)

s = +"ab"
t = +"xy"
s.concat(t, s)
puts s, t

s = +"abcd"
s.concat(s[0, 2], s)
puts s

s = +"abcd"
v = s[1..]
s.concat(v, s)
v << "!"
puts s, v

s = +"ab"
s.concat("-", "+", s)
puts s
puts "-".frozen?, "+".frozen?

s = +"ab"
n = nil
s.concat(n&.to_s || "", s, "z")
puts s

s = +"ab"
w = +"cd"
s.concat(w.upcase, w, s + w)
puts s, w

# an argument that a later argument's evaluation changes is read as it is
# then: both appends see "zzy"
s = +"ab"
u = +"zz"
s.concat(u, (u << "y"; u))
puts s
s = +"ab"
v = +"q"
s.concat(v, (v << "r"; v), (v << "s"; v))
puts s, v

# two calls in one statement
def f(s) = s.concat(s, "1")
s = +"q"
puts f(s) + f(s)

# an argument built by a call that allocates
s = +"ab"
s.concat(s * 2, "k" * 3, s)
puts s

# a boxed argument
a = [+"p", 3, nil]
s = +"ab"
a.each { |x| s.concat(x.to_s, s) }
puts s

# the receiver is an ivar, the arguments its own reads
class H
  def initialize = @s = +"hi"
  def go = @s.concat(@s, @s)
end
puts H.new.go

# a frozen receiver raises after every argument ran
log = []
fz = "frozen"
begin
  fz.concat((log << 1; "a"), (log << 2; "b"))
rescue FrozenError => e
  puts e.class
end
p log

# arguments an ordering ran first (an element read, a call a later argument
# follows) are bytes: their handles are made at the staging and survive the
# allocations of the arguments after them, and the receiver that a later
# argument rebinds is still the one the call was made on
$c = +"g"
x = +"abc"
r = $c.concat(($c = +"n"; x[0]), x[1])
p r, $c

def churn2
  a = []
  i = 0
  while i < 50
    a << ("q" * (i + 3))
    i += 1
  end
  a.size
end
$c = +"g"
x = +"abcdef"
orig = $c
r = $c.concat(($c = +"n"; x[0]), x[1], x[2])
p r, orig, $c
$d = +"h"
orig = $d
r = $d.concat(x[3], ($d = +"m"; x[4]), x[5])
p r, orig, $d
$e = +"k"
orig = $e
r = $e.concat(x[0], x[1], ($e = +"z"; churn2; x[2]))
p r, orig, $e
@s = +"i"
def go(x)
  o = @s
  r = @s.concat(x[1], x[2], (@s = +"w"; x[3]))
  p r, o, @s
end
go(x)
h = {k: +"h1"}
r = h[:k].concat(x[0], (h[:k] = +"h2"; x[1]), x[2])
p r, h
