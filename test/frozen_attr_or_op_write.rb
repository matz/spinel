# A frozen receiver refuses the attribute writer that `o.x ||= v`,
# `o.x &&= v` and `o.x op= v` run, as `o.x = v` does. A store that does
# not run (a set `||=`, a nil `&&=`) raises nothing.
class F
  attr_accessor :work, :name, :count, :list, :n, :s
  def initialize; @count = nil; @name = nil; @n = 1; @s = "a"; end
  def bump = (self.work ||= 3)
end
class Sub < F; end
class W
  attr_writer :w
  def w = @w
end
S = Struct.new(:a, :b)

def try(label)
  r = yield
  puts "#{label}: ok #{r.inspect}"
rescue FrozenError => e
  puts "#{label}: #{e.class}"
end

f = F.new.freeze
try("or poly") { f.work ||= 1 }
try("or str") { f.name ||= "x" }
try("or int") { f.count ||= 3 }
try("or arr") { f.list ||= [1] }
try("and nil") { f.work &&= 5 }
try("value") { v = (f.work ||= 9); v }
try("self in method") { f.bump }
try("op int") { f.n += 1 }
try("op str") { f.s += "b" }
try("op value") { x = (f.n -= 1); x }
try("sub") { Sub.new.freeze.work ||= 1 }
try("def reader") { W.new.freeze.w ||= 1 }
try("struct") { S.new(nil, 2).freeze.a ||= 1 }
try("struct set") { S.new(nil, 2).freeze.b ||= 1 }

g = F.new
g.work = 1
g.name = "a"
g.count = 2
g.freeze
try("or set") { g.work ||= 7 }
try("or set str") { g.name ||= "z" }

def m(o) = (o.count ||= 11)
try("param frozen") { m(F.new.freeze) }
try("param") { m(F.new) }
h = F.new
h.work ||= 1
h.count ||= 5
h.n += 2
h.s += "c"
p [h.work, h.count, h.n, h.s]
