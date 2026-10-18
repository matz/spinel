# A format chosen through an `||` chain of 11 operands, and through 10
# conditionals nested in each other's `else`, every value position of which is a
# constant or a read out of a slot. The recursion that asked whether something
# else holds the format stopped at a depth of 8 and answered "not held" past it,
# so the format was taken for a String made where it is written, and put in a
# temp of its own and rooted. The chain is read to its end now, and the format
# is held by what holds it.
# spinel: gc-stress
F = "%d-%s"
G = "[%d:%s]"
class Row
  attr_reader :fmt
  def initialize(id); @id = id; @fmt = "<%d|%s>"; end
  def id; @id; end
end

[1, 4, 11, 12].each do |id|
  row = Row.new(id)
  puts (row.fmt || F || G || F || G || F || G || F || G || F || G) % [id, "z"]
  puts (row.id == 10 ? G : (row.id == 9 ? F : (row.id == 8 ? G : (row.id == 7 ? F : (row.id == 6 ? G : (row.id == 5 ? F : (row.id == 4 ? G : (row.id == 3 ? F : (row.id == 2 ? G : (row.id == 1 ? F : F)))))))))) % [id, "z"]
end
