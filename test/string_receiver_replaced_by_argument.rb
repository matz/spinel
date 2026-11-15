# A String receiver whose argument replaces its content (here growing it, so
# the payload moves) reads the String as the argument left it, as CRuby
# does: the receiver is the String object, its bytes taken at the call.
# Read through a local, the receiver's bytes were taken before the argument
# ran, and the call read the old payload.

class Ctx
  attr_reader :buf
  def initialize = @buf = +"abc"
end

def f(ctx)
  ctx.buf.replace("xyz" * 200)
  1
end

ctx = Ctx.new
p ctx.buf.getbyte(f(ctx))
ctx = Ctx.new
b = ctx.buf
p b.getbyte(f(ctx))

# A method that changes its receiver, given an argument that grows the
# String the receiver names (an instance variable, the value of `||=`),
# changes the grown String and answers by it.
class Grow
  def initialize = @s = +"abc"
  def run = [@s.chomp!((@s << "x" * 300; "Z")), @s.size]
end
p Grow.new.run
gb = nil; gt = +"abc"
r = (gb ||= gt).chomp!((gb << "x" * 300; "x"))
p [r.size, gb.size, gt.size]
