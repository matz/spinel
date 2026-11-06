# A table's reads answer Integer rows when each value stored into it is one.
# `@rows[0] = @other[0]` stores a row of a second table, and `@other[0] =
# @mixed[i]` puts a String in that one. After the fixpoint the boxed @mixed
# is cleared and derived again, and @other's check ran in between: it took
# `@mixed[i]`, which had no type yet, for no value at all, so @other counted
# as a table of rows and @rows was read as one. The String's bytes were read
# as an Integer row and the program crashed. A stored value read out of a
# variable with no type yet is now taken as one that may be no row.
# infer-test greps the C: Through's @rows stays boxed, AllRows' narrows.
class Through
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed[i])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
  def other0 = @other[0]
end

t = Through.new([[7, 8], "row"])
t.fill(1)
t.take
p t.first
p t.other0
t.fill(0)
t.take
p t.first

# through a local, and with `<<` and `at`
class ViaLocal
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i)
    x = @mixed.at(i)
    @other << x
  end

  def take(j) = (@rows[0] = @other[j])
  def first = @rows[0]
end

v = ViaLocal.new([[5, 6], :sym])
v.fill(1)
v.take(1)
p v.first
v.fill(0)
v.take(2)
p v.first

# an element of an element of the mixed Array
class Deep
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed[i][0])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

d = Deep.new([[[9, 9]], ["deep"]])
d.fill(1)
d.take
p d.first
d.fill(0)
d.take
p d.first

# the stored value in other shapes: each class has its own tables, as one
# boxed table would hide the others

# the mixed Array itself
class Bare
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed)
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Bare.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# fetch
class Fetch
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed.fetch(i))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Fetch.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# dig
class Dig
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed.dig(i))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Dig.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# first and last
class Last
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = i == 1 ? @mixed.last : @mixed.first)
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Last.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# in parentheses
class Paren
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = (@mixed[i]))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Paren.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an arm of a ternary whose other arm is a row
class Ternary
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = i > 5 ? [1] : @mixed[i])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Ternary.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# the left of ||
class OrRow
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = @mixed[i] || [0])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = OrRow.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an arm of a case
class CaseArm
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = case i when 9 then [1] else @mixed[i] end)
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = CaseArm.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# a local in a ternary arm
class LocalArm
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (x = @mixed[i]; @other[0] = i > 5 ? [1] : x)
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = LocalArm.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an assignment to a local
class WriteLocal
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = (x = @mixed[i]))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = WriteLocal.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an assignment in a ternary arm
class WriteArm
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = i > 5 ? [1] : (x = @mixed[i]))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = WriteArm.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an assignment to another ivar
class WriteIvar
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = (@last = @mixed[i]))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = WriteIvar.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an ||= of a local
class WriteOr
  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = (y ||= @mixed[i]))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = WriteOr.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an attribute reader of the mixed Array
class Reader
  attr_reader :mixed

  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = mixed[i])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = Reader.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# the same reader called on self
class ReaderSelf
  attr_reader :mixed

  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = self.mixed[i])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = ReaderSelf.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# fetch on the reader
class ReaderFetch
  attr_reader :mixed

  def initialize(mixed)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @mixed = mixed
  end

  def fill(i) = (@other[0] = mixed.fetch(i))
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = ReaderFetch.new([[7, 8], "row"])
o.fill(1)
o.take
p o.first

# an attribute reader of another object
class Source
  attr_reader :value

  def initialize(value) = (@value = value)
end

class OtherReader
  def initialize(source)
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @source = source
  end

  def fill(i) = (@other[0] = @source.value[i])
  def take = (@rows[0] = @other[0])
  def first = @rows[0]
end

o = OtherReader.new(Source.new([[7, 8], "row"]))
o.fill(1)
o.take
p o.first

# every row an Integer Array: the tables still read rows
class AllRows
  def initialize
    @rows = [[1, 2]]
    @other = [[3, 4]]
    @src = [[5, 6], [7, 8]]
  end

  def fill(i) = (@other[0] = @src[i])
  def take = (@rows[0] = @other[0])
  def total = @rows[0].sum + @other[0].sum
end

a = AllRows.new
a.fill(1)
a.take
p a.total
