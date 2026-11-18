# spinel: gc-stress
# A frozen typed Array keeps its element display when a store raises.
class Cell
  def initialize(value) = @value = value
  def inspect = "Cell(#{@value})"
end

CELLS = [Cell.new(1), Cell.new(2)].freeze
begin
  CELLS << Cell.new(3)
rescue FrozenError => error
  puts error.message
end
