# A yielding method called without a block. A `return nil unless
# block_given?` nested under another condition stops only the calls that
# reach it; a blockless call that skips it runs on to the rest of the body.
# And a call whose only possible value is nil is still inlined: a yielding
# method has no standalone function to call.
class S
  def initialize(a)
    @a = a
  end

  def index(x = nil, &block)
    if x.nil?
      return nil unless block_given?
      return @a.find_index(&block)
    end
    @a.find_index { |v| v == x }
  end

  def first_match
    return nil unless block_given?
    @a.find { |v| yield v }
  end

  def pick(x)
    if x > 0
      return nil unless block_given?
      yield x
    end
    x * 10
  end
end

s = S.new([1, 2, 3])
p s.index(2)
p s.index(9)
p s.index
p s.index { |v| v == 3 }
p s.first_match
p s.first_match { |v| v > 1 }
p s.pick(0)
p s.pick(1)
p s.pick(1) { |v| v + 100 }
x = s.first_match
p x
