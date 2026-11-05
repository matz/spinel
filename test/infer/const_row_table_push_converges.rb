# A constant table of Float or Integer rows that a push also fills stays a
# table of typed rows, and the fixpoint settles.
FLOATS = [[1.0, 2.0]]
INTS = [[1, 2]].freeze
MORE = [[5, 6]]

def first_float(table) = table[0][0]
def first_int(table) = table[0][0]
def first_more(table) = table[0][0]

p first_float(FLOATS)
FLOATS << [3.0]
p FLOATS.size
p first_int(INTS)
begin
  INTS << [3]
rescue FrozenError
  puts "frozen"
end
p first_more(MORE)
MORE.push([7, 8], [9, 10])
p MORE.size
