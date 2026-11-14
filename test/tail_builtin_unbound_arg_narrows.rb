# A method whose last expression is a builtin's call on an argument no call
# had bound yet (`1.0 / (i + 1.0)` before any caller typed `i`) was taken for
# a call nothing resolves, and its return was typed boxed for that round.
# Where its value comes back as an argument -- a vector the next step reads,
# an accumulator handed on -- the parameter, which only widens, kept every
# slot of the loop boxed. The loop now stays Float.

def weight(i, j) = 1.0 / ((i + j) * (i + j + 1.0) / 2.0 + i + 1.0)

# the result of each step is the next step's input
def step(u)
  (0...u.size).map do |i|
    v = 0.0
    u.each_with_index { |x, j| v += weight(i, j) * x }
    v
  end
end

u = Array.new(6, 1.0)
4.times { u = step(u) }
p u.map { |x| x.round(9) }

# an accumulator the next call takes back
def scale(i) = 2.0 / (i + 1)
def acc(u, i)
  v = 0.0
  u.each { |x| v += x * scale(i) }
  v
end
w = [1.0, 2.0]
3.times { |k| w = [acc(w, k), acc(w, k + 1)] }
p w

# held in ivars, as a class's state
class Power
  def initialize(n) = (@u = Array.new(n, 1.0); @v = Array.new(n, 1.0))
  def at(i, j) = 1.0 / (i + j + 1)
  def mul(u) = (0...u.size).map { |i| s = 0.0; u.each_with_index { |x, j| s += at(i, j) * x }; s }
  def run = (@v = mul(@u); @u = mul(@v); nil)
  def ratio = (@u.sum / @v.sum).round(9)
end
pw = Power.new(5)
3.times { pw.run }
p pw.ratio

# a tail call no builtin answers still raises when its value is used
def unknown_tail(x) = 1.0.frobnicate(x)
begin
  p unknown_tail(1)
rescue NoMethodError => e
  p e.class
end
