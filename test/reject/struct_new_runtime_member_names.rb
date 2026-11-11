# Struct.new(*names) with names only the run time knows: the class would get
# no members and `new` would reject every keyword, so it is refused at
# compile time instead of compiling to an empty Struct (#11342). A list the
# compiler can read (literal symbols, `%w[...].map(&:to_sym)`) works.
names = ARGV.empty? ? %w[a b] : ARGV
Row = Struct.new(*names.map(&:to_sym), keyword_init: true)
p Row.members
