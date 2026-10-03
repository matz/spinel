# The block Array#fetch runs for an index out of bounds sees that index:
# what its value needs built first (an array literal's elements, an operand
# kept across a call that allocates) is built after the parameter is bound.
def mk(n); [n, n + 1]; end

a = [10, 20]
p a.fetch(7) { |i| [i, i + 1] }           # [7, 8]
p a.fetch(1) { |i| [i, i + 1] }           # 20
p a.fetch(7) { |i| i * mk(i).length }     # 14
p a.fetch(7) { |i| t = [i, i + 1]; t.sum }        # 15
p a.fetch(7) { |i| next mk(i) if i > 5; [0] }     # [7, 8]
p a.fetch(7) { }                                  # nil
s = ["x", "y"]
p s.fetch(5) { |i| [i.to_s, "k"] }        # ["5", "k"]
p s.fetch(5) { |i| t = i + 1; t.to_s + mk(t).length.to_s }   # "62"

# a receiver that is an Array or a Hash, decided at run time
class Bus
  def initialize(flag)
    @h = flag ? { a: 1, "s" => 2 } : [10, 20]
  end
  def pair = @h.fetch(7) { |i| [i, i * 2] }
  def scaled = @h.fetch(7) { |i| i * mk(i).length }
end
p Bus.new(true).pair, Bus.new(false).pair       # [7, 14] twice
p Bus.new(true).scaled, Bus.new(false).scaled   # 14 twice
