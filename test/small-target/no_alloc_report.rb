# SP_NO_ALLOC_REPORT: a program that builds Arrays, Hashes and objects
# compiles with the type-name tags fenced out of sp_tu_init.
class Point
  def initialize(x, y); @x = x; @y = y; end
  def sum; @x + @y; end
end
h = { "a" => [1, 2], "b" => [3] }
puts h.map { |k, v| "#{k}=#{v.sum}" }.join(",")
puts Point.new(3, 4).sum
