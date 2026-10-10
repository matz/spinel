# A dispatch table of bound Methods that also holds an Integer row with a
# nil in it, read as `@t[i][j]`: the Method is called, the row's element is
# read, and its nil comes back as nil (and as 0 through to_i).
class Table
  def initialize
    @t = [method(:double)]
    @t << [1, nil, 3]
  end
  def double(a) = a * 2
  def get(i, j) = @t[i][j]
  def add(i, j) = @t[i][j] + 1
  def int(i, j) = @t[i][j].to_i
end
t = Table.new
p t.get(0, 4)
p t.get(1, 0)
p t.get(1, 1)
p t.get(1, 9)
p t.add(1, 2)
p t.int(1, 1)
p t.int(1, 9)
p t.int(0, 5)
