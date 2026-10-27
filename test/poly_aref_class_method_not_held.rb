# A class's own `self.[]` takes a boxed receiver's [] only where the
# receiver can hold that Class value (#5545): an index read of a value that
# is an Array, a Hash or a String, in a program where no such Class value
# reaches it, is the builtin's, by a key or by a start and a length.
class Bag
  def self.[](*items) = new
end

rows = [[10, 20, 30], {"a" => 1}, "abc"]
p rows[0][1]
p rows[0][0, 2]
p rows[1]["a"]
p rows[1][:b]
p rows[2][1, 2]

def read(row, key) = row[key]
p read(rows[1], "a")
p read(rows[0], 2)
