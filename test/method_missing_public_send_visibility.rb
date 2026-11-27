# public_send reaches a public method only, and a private method answers a call
# on self only: method_missing takes the rest, as in CRuby.
class G
  attr_reader :secret, :open
  private attr_writer :secret
  attr_writer :open
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(name, include_private = false) = true
  def a = public_send(:secret=, 5)
  def b = self.public_send(:secret=, 6)
  def c = public_send(:open=, 7)
  def d = public_send(:hid, 8)
  def e = self.public_send(:hid, 9)
  def f = public_send(:zzz=, 10)
  def g = hid(11)
  def h = self.secret = 12
  private def hid(x) = x
end
class H < G; end
o = [G, H][ARGV.size].new
p o.a, o.secret
p o.b, o.secret
p o.c, o.open
p o.d, o.e, o.f, o.g
p o.h, o.secret
p o.public_send(:secret=, 13), o.secret
p o.public_send(:hid, 14)
p o.hid(15)
p o.public_send(:open=, 16), o.open
p o.send(:hid, 17), o.send(:secret=, 18), o.secret

class Txt < String
  def method_missing(n, *a) = "mm:#{n}"
  def respond_to_missing?(*) = true
  private def length = 99
  def go = public_send(:length)
end
p Txt.new("ab").go, Txt.new("ab").public_send(:length)

class Hidden
  private def method_missing(n, *a) = "mm:#{n}"
  private def respond_to_missing?(*) = true
  private def hid = 1
  def go = public_send(:hid)
end
p Hidden.new.go, Hidden.new.public_send(:hid), Hidden.new.zzz

# A protected writer takes a call on another receiver of its class; public_send still misses it.
class P
  attr_reader :v
  protected attr_writer :v
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(*) = true
  def set(o, x) = o.v = x
  def pset(o, x) = o.public_send(:v=, x)
  def nest = public_send(:hid, public_send(:hid, 1))
  private def hid(x) = x
end
pa, pb = P.new, [P, P][ARGV.size].new
p pa.set(pb, 3), pb.v
p pa.pset(pb, 4), pb.v
p pa.nest
# From outside the class line, a protected writer or method misses: method_missing takes it.
class Q
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(*) = true
  protected attr_writer :w
  protected def pm = 1
end
class Out
  def poke(q) = [(q.w = 5), q.pm]
end
p Out.new.poke([Q, Q][ARGV.size].new)
p [Q, Q][ARGV.size].new.pm
# A protected method is open to a caller whose self is of the method's owner, a sibling subclass included.
class Base
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(*) = true
  protected def token = 7
end
class Left < Base
  def read(o) = o.token
end
class Right < Base; end
p Left.new.read([Right, Right][ARGV.size].new)
# Inside instance_eval, self is the receiver: a protected method answers that caller.
class Qe
  def method_missing(name, *args) = "mm:#{name}"
  def respond_to_missing?(*) = true
  protected def pm = 2
end
class OutE
  def poke(o) = Qe.new.instance_eval { o.pm }
end
p OutE.new.poke([Qe, Qe][ARGV.size].new)
