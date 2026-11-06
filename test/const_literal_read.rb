# spinel: int64
# A constant bound once to an Integer or Float literal reads as the literal:
# in a loop that stores through an Array, and through a path; a read before
# its assignment still raises NameError, and a Bignum literal still reads
# its slot.
class Grid
  W = 8
  SCALE = 0.5
  NEG = -3
  def self.fill(cells, rows)
    j = 0
    while j < rows.length
      k = rows[j]
      cells[k * W + j % W] += 1
      j += 1
    end
    cells
  end
end

cells = Grid.fill(Array.new(8 * 8, 0), [0, 1, 2, 3, 4, 5, 6, 7, 1, 2])
p cells.sum
p cells.each_index.select { |i| cells[i] > 0 }
p Grid::W * Grid::SCALE
p Grid::NEG * Grid::W

class Late
  def self.size; SIZE * 2; end
end
begin
  Late.size
rescue NameError => e
  puts e.message
end
class Late
  SIZE = 16
end
p Late.size

BIG = 123456789012345678901234567890
p BIG + 1

