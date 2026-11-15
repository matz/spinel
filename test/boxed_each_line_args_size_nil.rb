# A boxed String's each_line with a separator or chomp: is CRuby's
# Enumerator, which has no size: .size is nil. Its count and to_a still
# give the lines.
x = ["a,b,c", 1].first
p x.each_line(",").size
p x.each_line(chomp: true).size
p x.each_line(",", chomp: true).size
p x.each_line("").size
p x.each_line(",").count
p x.each_line(",").to_a.size
p x.each_line(",").map(&:upcase)
p x.each_line(",").size.nil?
puts "size: #{x.each_line(",").size.inspect}"
h = { s: "l1\nl2\n" }
p h[:s].each_line(chomp: true).size
def lines_size(v) = v.each_line("\n").size
p lines_size("p\nq\n"), lines_size(["r\n", 2].first)
class Holder
  def initialize(v) = @v = v
  def size_of = @v.each_line(",").size
end
p Holder.new(["u,v", :k].first).size_of
y = [1, "z"].first
begin
  y.each_line(",").size
rescue NoMethodError => e
  p e.class
end
z = [nil, "a,b"].first
begin
  z&.each_line(",").size
rescue NoMethodError => e
  p e.message
end
