# fill with a block on an array of mixed elements: the block's value is
# built once for each index, ahead of the store it is written into.
def mk(n); [n, n + 1]; end

class Bus
  def initialize(flag)
    @pages = flag ? Array.new(4, 0) : [nil, "x", 1, 2]
  end
  def scale = @pages.fill(1) { |i| i * mk(i).length }
  def pages = @pages
end
[true, false].each do |f|
  b = Bus.new(f)
  b.scale
  p b.pages          # [0, 2, 4, 6], then [nil, 2, 4, 6]
end

# the block runs once for each index
$calls = 0
def tick(i); $calls += 1; [i, "t"]; end
m = [nil, "x", 1]
m.fill { |i| tick(i) }
p m, $calls          # [[0, "t"], [1, "t"], [2, "t"]], 3
m.fill(1) { |i| [i, i.to_s] }
p m                  # [[0, "t"], [1, "1"], [2, "2"]]
