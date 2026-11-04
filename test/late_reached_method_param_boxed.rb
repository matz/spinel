# A method the last reachability pass reaches first (Sel#matches?, a
# by-name call's once that pass opens Sel's instances) gets its unbound
# parameter boxed and its reads typed with it: `n` went raw into
# byteslice's sp_int slot, and the C did not build (#10913, openssl's
# SSLSocket#read_nonblock and sysread_nonblock's FFI int).
class Ctx
  def initialize
    @v = 1
  end
  def v = @v
end

class Sel
  def initialize(k, ctx = nil)
    @k = k
    @ctx = ctx.nil? ? Ctx.new : ctx
  end

  def matches?(n) = take(n)
  def take(n) = "abcdef".byteslice(0, n)

  def kind
    @k
  end
end

class Frag
  def self.parse(t)
    Sel.new(t)
  end

  def self.matches?(m)
    m.kind == "all"
  end
end

class Z
  def self.go
    Frag.matches?(Sel.new("all"))
  end
end

class Entry
  def matches?(name)
    name == "x"
  end
end

list = [Entry.new]
p list[0].matches?("x")
p Frag.name
