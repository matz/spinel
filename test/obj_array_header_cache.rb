# spinel: gc-stress
# A loop whose body runs no code holds an Array of objects' (or a table's)
# elements and length in C locals, as it does an Integer or Float Array's:
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
