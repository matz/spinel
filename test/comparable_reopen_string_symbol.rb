# A method the program adds to `module Comparable` answers for a String and
# a Symbol, as it does for an Integer and a Float. A String's own method of
# the name, and a builtin String or Symbol method of the name, answer first.
module Comparable
  def cx = 1
  def tag = "cmp:#{self}"
  def lohi(o) = (self < o ? :lo : :hi)
  def size = :cmp_size
  def mine = :cmp_mine
  def k = self.to_s + "?"
  def pr = (puts "pr"; 1)
  def tw = cx + cx
end

class String
  def mine = :str_mine
end

p "s".cx, :s.cx, 3.cx, 2.5.cx
p "b".tag, :b.tag
p "a".lohi("b"), :b.lohi(:a)
p "abc".size, :abc.size
p "s".mine, :s.mine, 3.mine
s = "x" * 2
p s.tag
h = {"k" => 1}
h.each_key { |k| p k.tag }
p(([1].cx rescue $!.class))
p "s".k, :s.k
p "s".pr
p "s".tw, :s.tw
