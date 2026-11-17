# A String Range held by a constant answers find_index(v) as a literal one
# does: the position, or nil on a miss.
R = ("a".."e")
p R.find_index("z")
p R.find_index("d")
k = R.find_index(ARGV.size == 0 ? "b" : "q")
p(k + 1)
m = R.find_index("q")
p m.nil?
