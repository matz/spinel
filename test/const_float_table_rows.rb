# Rows of a constant table of Float arrays, read through a method parameter:
# the values, a row's own methods, and arithmetic on the elements. FROZEN
# asks for its frozen state, which keeps it on the boxed path.
WEIGHTS = [[0.5, 1.5, 2.5], [10.0, 20.0, 30.0]].freeze
PLAIN = [[1.0, 2.0], [3.0, 4.0]]
FROZEN = [[1.0], [2.0]].freeze

def blend(table, row, at) = table[row][at] * 2.0 + table[row][0]

def row_of(table, row) = table[row]

p blend(WEIGHTS, 0, 1)
p blend(WEIGHTS, 1, 2)
p blend(PLAIN, 1, 0)
p row_of(WEIGHTS, 1)
p row_of(PLAIN, 0).map { |value| value / 2.0 }
p WEIGHTS.map(&:max)
p FROZEN.frozen?, FROZEN[0].frozen?
