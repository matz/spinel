# A true `Array[Integer]` seed pins @storage, so a `replace` with a source of
# another kind (a helper's boxed result, a general Array) cannot widen the
# receiver: the source converts to the receiver's kind. With no arm the call
# fell to NoMethodError. nil is Ruby's TypeError.
class SeedReplaceMem
  def initialize(initial = [])
    @storage = fill(initial)
  end

  def clear!(initial = [])
    @storage.replace(fill(initial))
  end

  def clear_quiet!(initial = [])
    @storage.replace(fill(initial))
    nil
  end

  def reset!(src) = @storage.replace(src)

  def [](i) = @storage[i]
  def size = @storage.size

  private

  def fill(initial)
    array = initial.dup
    0.upto(3) { |i| array[i] ||= 0 }
    array
  end
end

m = SeedReplaceMem.new([7])
m.clear!([5, 6])
p m[0] + m[1]
m.clear_quiet!([1, 2, 3, 4, 5])
p m.size
m.reset!([9, 8, 7, 6, 5, 4])
p m[5]
begin
  m.reset!(nil)
rescue TypeError => e
  p e.class
end
