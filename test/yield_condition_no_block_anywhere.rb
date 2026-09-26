# `yield` tested as a condition in a method no call site gives a block.
# The yield has no value type; the branch is only reached without a block,
# where it raises LocalJumpError, so the method still compiles.
class S
  def initialize(a)
    @a = a
  end

  def index(x = nil)
    i = 0
    while i < @a.length
      if x.nil?
        return i if yield(@a[i])
      elsif @a[i] == x
        return i
      end
      i += 1
    end
    nil
  end

  def any_match?
    @a.each { |v| return true if yield(v) }
    false
  end
end

s = S.new([1, 2])
p s.index(2)
p s.index(5)
begin
  s.index
rescue LocalJumpError => e
  p e.message
end
begin
  s.any_match?
rescue LocalJumpError => e
  p e.message
end
