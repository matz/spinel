# A typed Array's index answer read by to_s(base), in a program that
# reads no Integer as nil would not (the answer is the Integer that may be
# nil): nil's to_s takes no argument (ArgumentError); a found one prints
# in that base.
ia = [3, 1, 4]
miss = ARGV.size == 0 ? 99 : 4
x = ia.index(miss)
p((x.to_s(2) rescue $!.class))
p ia.index(4).to_s(2)
p ia.rindex(1).to_s(16)
