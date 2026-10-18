# A format chosen by an `elsif` ladder (`(if a then F elsif b then G else
# row.fmt end) % args`), every arm of which is a literal, a constant or a
# read out of a slot. The ladder was read as one choice only when it had
# no `elsif`, and past 8 arms it was not read at all: a format that a
# constant, a slot or a literal holds was taken for a String made where it
# is written, and put in a temp of its own and rooted. Every arm of
# the ladder is read now, and the format is held by what holds it.
# spinel: gc-stress
F = "%d-%s"
G = "[%d:%s]"
class Row
  attr_reader :fmt
  def initialize(id); @id = id; @fmt = "<%d|%s>"; end
  def id; @id; end
end

[1, 2, 3, 12, 13].each do |id|
  row = Row.new(id)
  puts (if row.id == 1 then F
       elsif row.id == 2 then G
       elsif row.id == 3 then F
       elsif row.id == 4 then G
       elsif row.id == 5 then F
       elsif row.id == 6 then G
       elsif row.id == 7 then F
       elsif row.id == 8 then G
       elsif row.id == 9 then F
       elsif row.id == 10 then G
       elsif row.id == 11 then F
       elsif row.id == 12 then G
        else row.fmt
        end) % [id, "z"]
end
