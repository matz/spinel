# Methods nothing names are blanked before the analysis when reading them
# tells the reached code nothing (#11708): no slot written, no method of
# the program called. The rest of the program answers as before: a method
# named by a symbol, a string or a call stays, so does one that writes an
# ivar, and the last statement of a body keeps its value.
class Toolkit
  def initialize(name) = (@name = name)
  def name = @name

  def unused_a(xs, n)
    ys = xs.map { |x| x * 3 + n }
    zs = ys.select { |y| y.odd? }.each_with_index.map { |y, i| y + i }
    "unused_a: #{zs.sum} #{zs.length}"
  end

  def unused_writer(v)
    @name = v.to_s * 2
  end

  def by_symbol(x) = x + 1
  def by_string(x) = x * 2

  def last_one = :kept
end

v = class Box
  def unused_b = [1, 2].sum
  def tail_value = 1
end

tk = Toolkit.new("t")
puts tk.name
p tk.respond_to?(:by_symbol)
p tk.respond_to?("by_string")
p tk.method(:by_symbol).call(1)
p v
p Toolkit.instance_method(:last_one).name
