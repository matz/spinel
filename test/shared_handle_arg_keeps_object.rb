# A shared String argument the call runs first -- because a later argument
# may reassign the variable -- binds the OBJECT it read, not a fresh String of
# its bytes. Bound as a copy, the callee held a second String: a write the
# caller made through its own name afterwards never reached it, and a context
# built over a buffer read stale bytes for everything written after.
class Opts
  attr_reader :threads
  def initialize
    @threads = 1
  end
end
class Ctx
  attr_reader :buf
  def initialize(buf, n)
    @buf = buf
    @n = n
  end
end
# (a) a later argument that is a plain field read
class Owner
  def initialize(buf)
    @buf = buf
    @opts = Opts.new
  end
  def ctx = Ctx.new(@buf, @opts.threads)
  def write(i, v) = @buf.setbyte(i, v)
end
o = Owner.new("\0" * 4)
c = o.ctx
o.write(1, 65)
p c.buf.getbyte(1)
# (c) a later argument that really reassigns the ivar: the call keeps the
# object read first, and a write through another name of it still shows
class Swapper
  def initialize(buf)
    @buf = buf
  end
  def swap
    @buf = +"zzzz"
    2
  end
  def ctx = Ctx.new(@buf, swap)
  def write(i, v) = @buf.setbyte(i, v)
end
orig = "\0" * 4
w = Swapper.new(orig)
c2 = w.ctx
orig.setbyte(0, 66)
w.write(0, 67)
p [c2.buf.getbyte(0), orig.getbyte(0)]
# (b) a local rebound by a later argument
s = +"ab"
t = s
k = Ctx.new(s, (s = +"q"; 1))
t << "!"
p [k.buf, s, t]
# (d) a variable run first that is also lent to a byref slot, beside an
# argument that takes a String's handle for itself: the slot's parameter
# takes the handle too, so the handle parameter binds the variable's own
# String. While the variable stayed a plain String, the parameter bound a
# fresh one, and before that the other argument's temp, which its block had
# already closed (the C build stopped)
module Helper
  def self.open_into(io) = (io << "<div>"; nil)
end
Helper.open_into([]) if ARGV.size > 5   # a second caller makes io POLY
def plain_into(io) = (io << "p"; nil)
def h(k0, io, k) = (Helper.open_into(io); io << "h#{k0}#{k}"; nil)
buf = +""
d = String.new
plain_into(d)
h((buf << "ab"; buf.upcase!; buf.size), d, (d = String.new; 1))
p [d, buf]
