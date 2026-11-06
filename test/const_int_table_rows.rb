# Constant tables of Integer rows, frozen and not, at the top level and in a
# module, read through method parameters.
module Grid
  CELLS = [[1, 2, 3], [4, 5, 6]].freeze
  OFFSETS = [[10, 20], [30, 40]]

  def self.cell(table, row, column) = table[row][column]

  def self.row_total(table, row) = table[row].sum
end

SHIFTS = [[1, 1], [2, 2]].freeze

def shifted(table, row) = table[row].map { |value| value * 2 }

p Grid.cell(Grid::CELLS, 1, 2)
p Grid.cell(Grid::OFFSETS, 0, 1)
p Grid.row_total(Grid::CELLS, 0)
p Grid.row_total(Grid::OFFSETS, 1)
p shifted(SHIFTS, 1)
p shifted(Grid::CELLS, 0)
