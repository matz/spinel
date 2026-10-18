# A format chosen through a rescue modifier, every value position of which is a
# constant or a read out of a slot. The choice was not read, so the format
# was taken for a String made where it is written, and put in a temp of its
# own and rooted. Every position is read now, and the format is held by what
# holds it.
# spinel: gc-stress
F = "%d-%s"
G = "[%d:%s]"
class Row
  attr_reader :fmt
  def initialize(id); @id = id; @fmt = "<%d|%s>"; end
  def id; @id; end
end

[1, 2, 3].each do |id|
  row = Row.new(id)
  puts (row.fmt rescue F) % [id, "z"]
end
