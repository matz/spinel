# A method that yields is spliced at each call, so a block that calls the
# method again puts a second copy of its body inside the first, at the
# first's `yield`. An argument the first copy had run ahead of its call (the
# first of two yields, a call written before a yield) was still held for
# those nodes, and the second copy took that value for its own argument
# without running it: `M.k("p") { M.k("q") { "s" } }` answered p/p/s, and
# `M.pair { M.pair { "s" } }` five s for four.

LOG = []
def note(tag)
  LOG << tag
  tag
end
def log
  s = LOG.join(",")
  LOG.clear
  s
end

module C
  def self.cat(a, b) = "#{a}/#{b}"
  def self.str(v) = v.is_a?(String) ? v : v.inspect
  def self.two(a, b) = "#{str(a)}/#{str(b)}"
end

module M
  def self.pair = C.cat(yield, yield)
  def self.lead = C.cat(note("lead"), yield)
  def self.trail = C.cat(yield, note("trail"))
  def self.k(x) = C.cat(note(x), yield)
  def self.three = "#{C.cat(yield, yield)}/#{yield}"
  # a parameter of more than one type
  def self.mixed = C.two(yield, yield)
  def self.mixed_k(x) = C.two(note(x), yield)
  def self.stmts
    a = C.cat(yield, yield)
    b = C.cat(yield, note("t"))
    "#{a}|#{b}"
  end
  def self.in_block = [1, 2].map { |i| C.cat(yield(i), yield(i + 10)) }.join("+")
  def self.call(&blk) = C.cat(blk.call, blk.call)
  def self.fwd = pair { yield }
  def self.other = C.cat(yield, yield)
end

# two yields in one call
puts M.pair { note("o"); M.pair { note("i"); "s" } }
puts log
puts M.pair { note("o"); M.pair { note("m"); M.pair { note("i"); "s" } } }
puts log
puts M.three { note("o"); M.three { note("i"); "s" } }
puts log

# a call beside the yield, before it and after it
puts M.lead { note("o"); M.lead { note("i"); "s" } }
puts log
puts M.trail { note("o"); M.trail { note("i"); "s" } }
puts log
puts M.k("p") { note("o"); M.k("q") { note("i"); "s" } }
puts log

# blocks of more than one type
puts M.mixed { note("o"); M.mixed { note("i"); "s" } }
puts M.mixed { note("o"); M.mixed { note("i"); "s" }.length }
puts M.mixed { 7 }
puts log
puts M.mixed_k("p") { note("o"); M.mixed_k("q") { note("i"); 8 } }
puts M.mixed_k("r") { "s" }
puts log

# statements, a yield inside a block of the method, the block as a proc
puts M.stmts { note("o"); M.stmts { note("i"); "s" } }
puts log
puts M.in_block { |a| note("o#{a}"); M.in_block { |b| note("i#{b}"); "s" } }
puts log
puts M.call { note("o"); M.call { note("i"); "s" } }
puts log

# through a method that hands its block on, and through another method
puts M.fwd { note("o"); M.fwd { note("i"); "s" } }
puts log
puts M.pair { note("o"); M.other { note("m"); M.pair { note("i"); "s" } } }
puts log

# an instance method, on one receiver and on two
class K
  def initialize(n) = @n = n
  def pair = C.cat(yield(@n), yield(@n + 1))
end
a = K.new(1)
b = K.new(10)
puts a.pair { |x| note("o#{x}"); a.pair { |y| note("i#{y}"); "s#{x}#{y}" } }
puts log
puts a.pair { |x| note("o#{x}"); b.pair { |y| note("i#{y}"); "s#{x}#{y}" } }
puts log

# an initialize that yields
class Box
  attr_reader :v
  def initialize
    @v = C.cat(yield, yield)
  end
end
puts Box.new { note("o"); Box.new { note("i"); "s" }.v }.v
puts log

# super into a method that yields, bare and with a block of its own
class P
  def run = C.cat(yield, yield)
end
class Q < P
  def run = super
end
class R < P
  def run = super { yield }
end
q = Q.new
r = R.new
puts q.run { note("o"); q.run { note("i"); "s" } }
puts log
puts r.run { note("o"); r.run { note("i"); "s" } }
puts log

# one method body in two classes
module Mix
  def run = C.cat(yield, yield)
end
class A1; include Mix; end
class B1; include Mix; end
a1 = A1.new
b1 = B1.new
puts a1.run { note("o"); b1.run { note("i"); "s" } }
puts log

# a block that raises out of the second copy leaves the next call its own
r = begin
  M.pair { note("o"); M.pair { note("i"); raise "boom" if LOG.size > 3; "s" } }
rescue => e
  e.message
end
puts r
puts log
puts M.pair { note("p"); M.pair { note("q"); "s" } }
puts log
