# spinel: gc-stress
# A typed receiver takes the same operand snapshots in statement and value
# positions. A later conversion must not change an earlier boxed operand.
$source = [5, 6, 7, 8]
class ConcatMutator
  def to_ary
    $source.clear
    $source << 42
    GC.start
    [9]
  end
end
def concat_statement(a, x, y)
  a.concat(x, y)
  a
end
def concat_value(a, x, y)
  a.concat(x, y)
end
x = [$source, 0].first
y = [ConcatMutator.new, 0].first
p concat_statement([1, 2], x, y)
$source = [5, 6, 7, 8]
x = [$source, 0].first
p concat_value([1, 2], x, y)

# A boxed operand converts after the frozen test. Repeated boxed
# operands convert once each, in their argument order.
$log = []
class ConcatPair
  def initialize(n)
    @n = n
  end
  def to_ary
    $log << @n
    GC.start
    [@n]
  end
end
a = [1, 2].freeze
begin
  a.concat([ConcatPair.new(3), 0].first)
rescue => e
  p e.class
end
p $log
b = [1, 2]
p b.concat([ConcatPair.new(3), 0].first, [ConcatPair.new(4), 0].first)
p $log

# Every argument runs before conversions begin, and the answer is the
# receiver. Repeated self operands read its length before any append.
$target = [1, 2]
def concat_argument
  $target << 3
  [9]
end
p $target.concat($target, concat_argument)
c = [1, 2]
p c.concat(c, c).equal?(c)
p c

# Value calls preserve nils and shifted storage, including self aliasing.
def concat_bulk(a, b)
  a.concat(b)
end
d = [1, 2, 3]
d[1] = nil
e = [4, 5, 6]
e.shift
p concat_bulk(e, d).equal?(e)
p e
p concat_bulk(d, d)

# Direct conversions keep their ordinary call plans, including the method
# version live at each call. Moving them to the implicit hook loses this.
class ConcatVersion
  def to_ary; [3]; end
end
v = ConcatVersion.new
p [1].concat(v)
class ConcatVersion
  def to_ary; [4]; end
end
p [2].concat(v)
