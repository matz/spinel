# A yield passed as an argument runs its block once. The call takes an
# argument that allocates, or that another argument has to run after, into a
# temp ahead of its binding; a parameter that holds more than one type (the
# method's blocks answer a String at one call site and an Integer at another)
# then boxed the yield by running the block where the parameter is bound, a
# second time: the block of `C.str(yield)` appended to LOG twice.

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
  def self.str(v)
    case v
    when nil then ""
    when String then v
    else v.to_s
    end
  end
  def self.two(a, b) = "#{str(a)}/#{str(b)}"
  def self.same(v, w) = v.equal?(w)
  def self.app(v)
    v << "!" if v.is_a?(String)
    v
  end
end

class A
  def two(a, b) = "A:#{C.two(a, b)}"
end
class B
  def two(a, b) = "B:#{C.two(a, b)}"
end

module M
  def self.one = C.str(yield)
  def self.with_arg = C.str(yield(3))
  def self.pair = C.two(yield, yield)
  def self.before = C.two(yield, note("after"))
  def self.after = C.two(note("before"), yield)
  def self.same(w) = C.same(yield, w)
  def self.app = C.app(yield)
  def self.poly(o) = o.two(yield, note("arg"))
  def self.each2
    out = []
    [1, 2].each { |i| out << C.str(yield(i)) }
    out
  end
  # the block is a proc here: the method's own, and one handed on
  def self.fwd(&b) = one(&b)
  def self.fwd_pair(&b) = before(&b)
  def self.rec(n, &b)
    return C.str(yield) if n == 0
    rec(n - 1, &b)
  end
  # these ran their block once already
  def self.paren = C.str((yield))
  def self.call(&block) = C.str(block.call)
  def self.wrap = one { yield }
end

class K
  def str(v) = v.is_a?(String) ? v : v.inspect
  def one = str(yield)
end

def top = C.str(yield)

# a String block and an Integer one
a = M.one do
  note("a")
  "one"
end
b = M.one { 42 }
puts "#{a} #{b} #{log}"

a = M.one { note("b"); 42 }
b = M.one { note("c"); "two" }
c = M.one { note("d"); nil }
d = M.one { note("e"); :sym }
e = M.one { note("f"); +"buf" }
puts "#{a} #{b} #{c.inspect} #{d} #{e} #{log}"

a = M.with_arg { |x| note("g"); "v#{x}" }
b = M.with_arg { |x| note("h"); x * 2 }
puts "#{a} #{b} #{log}"

# beside another argument, each runs once and in the order written
a = M.pair { note("i"); "p" }
b = M.pair { note("j"); 5 }
puts "#{a} #{b} #{log}"
a = M.before { note("k"); "q" }
b = M.before { note("l"); 1.5 }
puts "#{a} #{b} #{log}"
a = M.after { note("m"); "r" }
b = M.after { note("n"); [1, 2] }
puts "#{a} #{b} #{log}"

# the String the block answers is the one the parameter holds
s = +"abc"
puts M.same(s) { note("o"); s }
puts M.same(4) { note("p"); 4 }
r = M.app { note("q"); s }
t = M.app { note("r"); 6 }
puts "#{s} #{r.equal?(s)} #{t} #{log}"

# a receiver of more than one class
[A.new, B.new].each do |o|
  puts M.poly(o) { note("s"); "u" }
  puts M.poly(o) { note("t"); 7 }
end
puts log

p M.each2 { |i| note("u#{i}"); "z#{i}" }
p M.each2 { |i| note("v#{i}"); i * 3 }
puts log

# a block that is a proc
str_proc = proc { note("w"); "pv" }
int_proc = proc { note("x"); 8 }
puts M.fwd { note("y"); "f" }
puts M.fwd { note("z"); 9 }
puts M.fwd(&str_proc)
puts M.fwd(&int_proc)
puts M.one(&str_proc)
puts M.one(&int_proc)
puts M.fwd_pair(&str_proc)
puts M.fwd_pair(&int_proc)
puts M.rec(2) { note("A"); "deep" }
puts M.rec(2) { note("B"); 10 }
puts log

# the method called again from its own block: each copy runs its own yields
a = M.pair { note("O"); M.pair { note("P"); "n" } }
b = M.pair { note("Q"); M.pair { note("R"); "n" }.length }
puts "#{a} #{b} #{log}"
a = M.before { note("S"); M.before { note("T"); "n" } }
b = M.after { note("U"); M.after { note("V"); 16 }.length }
puts "#{a} #{b} #{log}"
a = M.one { note("W"); M.one { note("X"); M.one { note("Y"); "n" } } }
b = M.one { note("Z"); M.one { note("a"); 17 } }
puts "#{a} #{b} #{log}"

# the forms that already ran their block once
a = M.paren { note("C"); "three" }
b = M.paren { note("D"); 11 }
puts "#{a} #{b} #{log}"
a = M.call { note("E"); "four" }
b = M.call { note("F"); 12 }
puts "#{a} #{b} #{log}"
a = M.wrap { note("G"); "five" }
b = M.wrap { note("H"); 13 }
puts "#{a} #{b} #{log}"
k = K.new
a = k.one { note("I"); "six" }
b = k.one { note("J"); 14 }
puts "#{a} #{b} #{log}"
a = top { note("K"); "seven" }
b = top { note("L"); 15 }
puts "#{a} #{b} #{log}"

# one type at every call site
def all_str = C.app(yield)
puts all_str { note("M"); +"x" }
puts all_str { note("N"); +"y" }
puts log
