# block_given? inside a block that is compiled as a proc (its receiver is
# boxed and a class of the program has an each), in a method with &block
class W
  def initialize = @h = {}
  def update(hash, &block)
    hash.each do |key, value|
      @h[key] = block_given? ? block.call(key, @h[key], value) : value
    end
    self
  end
  def each(&b)
    @h.each(&b)
    self
  end
  def to_s = @h.to_s
end
w = W.new
src = [{ "a" => 1 }, W.new]
w.update(src[0])
w.update(src[1]) { |k, o, n| n * 10 }
w.update({ "c" => 3 }) { |k, o, n| n }
puts w
