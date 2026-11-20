# A method's &block parameter is reachable from a Thread.new / Fiber.new block
# in that method's body. The block runs as an independent closure on its own
# fiber stack, so the method must keep a heap-materialized &blk (not be
# yield-inlined) and the call site must pass the lowered block proc. Previously
# the escape went undetected: the method was inlined and the thread closure
# referenced an undeclared lv_blk (C compile error), or -- for a class/module
# method -- the block was silently dropped at the call site.

# module (class-)method: &blk captured by a Thread.new block
module M
  def self.f(&blk)
    t = Thread.new { blk.call(0) }
    t.join
  end
end
M.f { |i| puts "got #{i}" }            # got 0

# instance method: same capture through a thread
class C
  def run(&blk)
    t = Thread.new { blk.call(7) }
    t.join
  end
end
C.new.run { |i| puts "run #{i}" }      # run 7

# a positional parameter ahead of &blk, forwarded into the thread
module N
  def self.g(prefix, &blk)
    t = Thread.new { blk.call(prefix) }
    t.join
  end
end
N.g("x") { |s| puts "pre #{s}" }       # pre x

# Fiber.new capture of the &blk param
module F
  def self.h(&blk)
    fib = Fiber.new { blk.call(3) }
    fib.resume
  end
end
F.h { |i| puts "fib #{i}" }            # fib 3

# class-method &blk used as a value (assigned to a local, then called): the
# call site must still pass the block proc (was dropped before the fix)
module V
  def self.take(&blk)
    p = blk
    p.call(9)
  end
end
V.take { |i| puts "val #{i}" }         # val 9

# class-method &blk with a value-returning call, summed across two invocations
module S
  def self.calc(&blk)
    blk.call(1) + blk.call(2)
  end
end
puts S.calc { |i| i * 10 }             # 30

# &blk read inside a block that is *lifted* to a standalone proc (passed to a
# method that keeps a real &block param): the same capture-escape as the
# Thread case, so the method must keep a heap &blk too
def taker(&t)
  t
end

def outer(&blk)
  pr = taker { blk.call(4) }
  pr.call
end
outer { |i| puts "lift #{i}" }         # lift 4

# a class method reached through a module singleton accessor must also pass
# the block proc -- Stage-1: the accessor is bound to exactly one constant
module Reg1
  class << self
    attr_accessor :handler
  end
end

class H1
  def self.run(x, &blk)
    t = Thread.new { blk.call(x) }
    t.join
  end
end

Reg1.handler = H1
Reg1.handler.run(5) { |v| puts "reg1 #{v}" }   # reg1 5

# Stage-2: the accessor is written with two distinct constants, so the call
# dispatches over the stored class -- each branch passes the same block proc
module Reg2
  class << self
    attr_accessor :handler
  end
end

class H2a
  def self.run(x, &blk)
    t = Thread.new { blk.call(x + 1) }
    t.join
  end
end

class H2b
  def self.run(x, &blk)
    t = Thread.new { blk.call(x + 2) }
    t.join
  end
end

Reg2.handler = H2a
Reg2.handler.run(10) { |v| puts "reg2a #{v}" } # reg2a 11
Reg2.handler = H2b
Reg2.handler.run(10) { |v| puts "reg2b #{v}" } # reg2b 12

curried = ->(v) { puts "curried #{v * 2}" }.curry
Reg2.handler = H2a
Reg2.handler.run(10, &curried)
Reg2.handler = H2b
Reg2.handler.run(10, &curried)
