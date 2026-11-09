# spinel: gc-stress
# A loop whose body runs no code holds an Array of objects' (or a table's,
# or an Array of Strings') elements and length in C locals, as it does an
# Integer or Float Array's:
# `while i < a.size` reading `a[i]`, and `a.each { }`. What is out of the
# held range -- a negative index, past the end, a nil receiver -- reads
# through the Array as before.
class User
  attr_reader :score
  def initialize(s) = @score = s
end

def total(users)
  s = 0
  i = 0
  while i < users.size
    s += users[i].score
    i += 1
  end
  s
end

def total_each(users)
  s = 0
  users.each { |u| s += u.score }
  s
end

# an index the loop does not bound: the next element, then the last one
# (negative), and past the end (nil)
def pairs(users)
  s = 0
  i = 0
  while i < users.size
    n = users[i + 1]
    s += n.score if n
    s += users[-1].score
    s += 100 if users[i + 5].nil?
    i += 1
  end
  s
end

# a nil element raises at its read, as CRuby's does
def total_nil(users)
  s = 0
  i = 0
  while i < users.length
    s += users[i].score
    i += 1
  end
  s
end

users = Array.new(10) { |i| User.new(i) }
p total(users), total_each(users), pairs(users)
p total([]), total_each([])
mixed = [User.new(1), nil, User.new(3)]
begin
  p total_nil(mixed)
rescue NoMethodError => e
  p e.message
end
begin
  p total(nil)
rescue NoMethodError => e
  p e.message
end

# a table of Integer rows: its rows' writes in place refresh nothing they
# do not move, and a row grown past its buffer is read again
g = Array.new(4) { |i| Array.new(3) { |j| i * 3 + j } }
t = 0
g.each { |row| t += row[0] + row[2] }
p t
i = 0
while i < g.size
  g[i][1] = g[i][1] * 10
  i += 1
end
p g
s = 0
g.each { |row| row[1] += 1; s += row[1] }
p s, g[3]

# a loop at size: the rows held across a collection
big = Array.new(2000) { |k| User.new(k % 13) }
p total(big), total_each(big)

# an Array of Strings: the element's bytes read where the header holds them
def chars(strs)
  s = 0
  i = 0
  while i < strs.size
    s += strs[i].size + strs[i].bytesize
    i += 1
  end
  s
end

def chars_each(strs)
  s = 0
  strs.each { |x| s += x.bytesize }
  s
end

def str_pairs(strs)
  s = 0
  i = 0
  while i < strs.size
    n = strs[i + 1]
    s += n.size if n
    s += strs[-1].size
    i += 1
  end
  s
end

words = %w[a bb ccc dddd] + ["\u00e9", "\u65e5\u672c"]
p chars(words), chars_each(words), str_pairs(words), chars([]), chars_each([])
begin
  p chars(["x", nil, "zz"])
rescue NoMethodError => e
  p e.message
end
begin
  p chars(nil)
rescue NoMethodError => e
  p e.message
end
many = Array.new(3000) { |k| "s" * (k % 9) }
p chars(many), chars_each(many)

# held through an ivar and through a field read; after a shift, whose
# elements start past the slots it stepped over
class Box
  attr_reader :items, :names
  def initialize(n)
    @items = Array.new(n) { |i| User.new(i) }
    @names = Array.new(n) { |i| "n" * (i % 4) }
  end

  def sum
    s = 0
    i = 0
    while i < @items.size
      s += @items[i].score + @names[i].size
      i += 1
    end
    @items.each { |u| s += u.score }
    s
  end
end

def sum_field(b)
  s = 0
  i = 0
  while i < b.items.size
    s += b.items[i].score + b.names[i].bytesize
    i += 1
  end
  s
end

box = Box.new(6)
p box.sum, sum_field(box)
sh = Array.new(8) { |k| User.new(k) }
sh.shift
sh.shift
sw = %w[p q r s t]
sw.shift
p total(sh), total_each(sh), chars(sw), chars_each(sw)
