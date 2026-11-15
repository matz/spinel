# A conditional attribute write (`o.x ||= v`, `o.x &&= v`) reads the reader
# and calls the writer the call plan binds at its own place, as the two
# calls it stands for do: where an accessor and a def of the name follow one
# another in the class, each half takes the version live at the write, not
# the last one the class ends with.

# the reader's version changes: a def that answers @y, then the accessor
class ReaderChanges
  def initialize(a, b); @x = a; @y = b; end
  def gx = @x
  def gy = @y
  def x = @y
  attr_writer :x
end
r = ReaderChanges.new(:a, nil)
q = (r.x ||= :one)
p [q, r.gx, r.gy]
class ReaderChanges
  attr_reader :x
end
r = ReaderChanges.new(:a, nil)
q = (r.x ||= :one)
p [q, r.gx, r.gy]

# the reader's version changes the other way, under `&&=`
class ReaderBack
  def initialize(a, b); @x = a; @y = b; end
  def gx = @x
  def gy = @y
  attr_reader :x
  attr_writer :x
end
r = ReaderBack.new(:a, :b)
q = (r.x &&= :one)
p [q, r.gx, r.gy]
class ReaderBack
  def x = @y
end
r = ReaderBack.new(:a, nil)
q = (r.x &&= :one)
p [q, r.gx, r.gy]

# the writer's version changes: a def that stores elsewhere, then the accessor
class WriterChanges
  def initialize(a, b); @x = a; @y = b; end
  def gx = @x
  def gy = @y
  attr_reader :x
  def x=(v)
    @x = :wdef
    @y = v
  end
end
w = WriterChanges.new(nil, nil)
q = (w.x ||= :one)
p [q, w.gx, w.gy]
class WriterChanges
  attr_writer :x
end
w = WriterChanges.new(nil, nil)
q = (w.x ||= :one)
p [q, w.gx, w.gy]

# the writer's version changes the other way, under `&&=`
class WriterBack
  def initialize(a, b); @x = a; @y = b; end
  def gx = @x
  def gy = @y
  attr_accessor :x
end
w = WriterBack.new(:a, nil)
q = (w.x &&= :one)
p [q, w.gx, w.gy]
class WriterBack
  def x=(v)
    @x = :wdef
    @y = v
  end
end
w = WriterBack.new(:a, nil)
q = (w.x &&= :one)
p [q, w.gx, w.gy]

# both halves change at one reopening
class BothChange
  def initialize(a, b); @x = a; @y = b; end
  def gx = @x
  def gy = @y
  def x = @y
  def x=(v)
    @x = :wdef
    @y = v
  end
end
b = BothChange.new(:a, nil)
q = (b.x ||= :one)
p [q, b.gx, b.gy]
class BothChange
  attr_accessor :x
end
b = BothChange.new(:a, nil)
q = (b.x ||= :one)
p [q, b.gx, b.gy]
b = BothChange.new(nil, nil)
q = (b.x ||= :one)
p [q, b.gx, b.gy]

# a def reader that only answers nil: the pair reads it for its nil, a
# subclass's later attr_reader does not hide it from the write before
class NilReader
  def x = nil
  def x=(v)
    @y = v
  end
  def y = @y
end
n = NilReader.new
q = (n.x ||= 3)
p [q, n.y]
q = (n.x &&= 4)
p [q, n.y]
class NilReaderSub < NilReader
  def initialize; @x = 5; end
end
s = NilReaderSub.new
s.x ||= 3
p s.y
class NilReaderSub
  attr_reader :x
end
s.x ||= 4
p s.y
