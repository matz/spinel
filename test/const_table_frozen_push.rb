# A frozen constant table that narrows to a typed table keeps its frozen
# state: a push, pop, shift or clear on it raises FrozenError, as in CRuby,
# and the error's receiver shows the table's elements.
class Cell
  def initialize(value) = (@value = value)
  def inspect = "Cell(#{@value})"
end

module Grid; end

TABLE = [[3.0, 2.0], [22.0, 20.0]].freeze
CELLS = [Cell.new(1), Cell.new(2)].freeze
Grid::ROWS = [[1, 2], [3, 4]].freeze

def land(table, row) = table[row][0]

def cell(table, row) = table[row][1]

p land(TABLE, 1)
begin
  TABLE << [1.0]
rescue FrozenError => e
  p e.class
end
p TABLE.size
begin
  CELLS << Cell.new(3)
rescue FrozenError => e
  p e.class
end
begin
  CELLS.pop
rescue FrozenError => e
  p e.class, e.receiver
end
begin
  CELLS.shift
rescue FrozenError => e
  p e.class
end
begin
  CELLS.clear
rescue FrozenError => e
  p e.class
end
p CELLS.size
p cell(Grid::ROWS, 1)
begin
  Grid::ROWS << [5, 6]
rescue FrozenError => e
  p e.class
end
p Grid::ROWS.size
