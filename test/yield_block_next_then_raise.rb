# A block that answers through `next` and otherwise raises: the call
# answers the `next` value where the `next` is taken, and what the
# method's rescue answers where the block raises.

def g
  yield
rescue => e
  e.message
end

def gi
  yield
rescue
  7
end

# a String, into a local, an argument and an interpolation
[1, 2].each do |i|
  x = g { next "n" if i == 1; raise "b" }
  p x
  p g { next "n" unless i == 2; raise "b" }
  puts "v=#{g { next "n" if i == 1; fail "b" }}"
end

# ...and where a String is kept
out = []
h = { "a" => "1" }
[1, 2].each do |i|
  out << g { next "n" if i == 1; raise "b" }
  out.push(g { next "m" if i == 1; raise ArgumentError, "c" })
  lit = [g { next "l" if i == 1; raise "d" }, "z"]
  p lit
  h["k#{i}"] = g { next "h" if i == 1; raise "e" }
end
p out, h

# an Integer
nums = [0]
hi = { "a" => 1 }
[1, 2].each do |i|
  x = gi { next 5 if i == 1; raise "b" }
  p x
  p(gi { next 5 if i == 1; raise "b" } + 1)
  nums << gi { next 6 if i == 1; raise "b" }
  hi["k#{i}"] = gi { next 8 if i == 1; raise "b" }
end
p nums, hi

# a Float, a Symbol, an Array, an object
def gf
  yield
rescue
  1.5
end
def gs
  yield
rescue
  :r
end
def ga
  yield
rescue
  [0]
end
class Box
  attr_reader :v
  def initialize(v) = @v = v
end
def gb
  yield
rescue
  Box.new(0)
end
[1, 2].each do |i|
  p gf { next 2.5 if i == 1; raise "b" }
  p gs { next :n if i == 1; raise "b" }
  p ga { next [i] if i == 1; raise "b" }
  p gb { next Box.new(3) if i == 1; raise "b" }.v
end

# a `next` of more than one kind
[1, 2, 3].each { |i| p g { next 5 if i == 1; next "s" if i == 2; raise "b" } }

# statements around the `next`, and a raise with nothing to raise again
def gk
  yield
rescue => e
  e.class.to_s
end
[1, 2].each do |i|
  p g { j = i * 2; next "n#{j}" if i == 1; j += 1; raise "b#{j}" }
  p gk { next "n" if i == 1; raise }
end

# a yield with an argument, a block parameter's call, an ensure
def gv(v)
  yield v
rescue => e
  e.message + v
end
def gc(v, &blk)
  blk.call(v)
rescue => e
  e.message
end
def ge
  yield
rescue => e
  e.message
ensure
  puts "ensure"
end
[1, 2].each do |i|
  p gv("q") { |s| next s + "n" if i == 1; raise "b" }
  p gc("q") { |s| next s + "n" if i == 1; raise "b" }
  p ge { next "n" if i == 1; raise "b" }
end

# a method with no rescue: the raise leaves the call
def t
  yield
end
seen = []
[1, 2].each do |i|
  begin
    seen << t { next "n" if i == 1; raise "b" }
    p t { next 5 if i == 1; raise "c" }
  rescue => e
    seen << e.message
  end
end
p seen

# a block that returns from the method around it where it does not `next`
def k(i)
  x = g { next "n" if i == 1; return "ret" }
  x + "!"
end
p k(1), k(2)
