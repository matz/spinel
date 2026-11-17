# A number's arithmetic with an object whose class has no #coerce raises
# TypeError, naming the object's class, after both operands ran. Inference
# types the operation as the number it would answer, as for a String or nil
# operand, so it can stand in a ternary's arm or a method's branch beside a
# number; it was untyped and the arm compiled to a box (or, on the Float
# side, to a double added to a pointer).
class Item
  attr_reader :v
  def initialize(v) = @v = v
end
class Special < Item; end

def total(o) = o.v > 1 ? o.v * 10 : 10 + o
def ratio(o) = o.v > 1 ? o.v * 0.5 : 2.5 * o

p total(Item.new(5)), ratio(Item.new(4))
[Item.new(0), Special.new(1)].each do |o|
  [-> { total(o) }, -> { ratio(o) }, -> { 2**70 - o }, -> { 3 % o }, -> { 1.5 ** o }].each do |f|
    p f.call
  rescue TypeError => e
    p e.message
  end
end

order = []
begin
  (order << :left; 1) + (order << :right; Item.new(2))
rescue TypeError => e
  p e.message, order
end
x = ARGV.empty? ? 7 : 1 + Item.new(1)
y = ARGV.empty? ? 0.5 : 1.5 - Item.new(1)
p x, y
