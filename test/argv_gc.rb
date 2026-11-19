# ARGV's byte views keep their String owners live through the global mark.
# The fixture's arguments are read before and after sustained allocation.

class Trash
  def initialize(n)
    @n = n
    @s = "padding payload " * 64    # ~1 KB heap string per Trash
  end
  attr_reader :n
end

# Allocate enough Trash instances to collect several times.
junk = []
j = 0
while j < 5000
  junk << Trash.new(j)
  j = j + 1
end
puts junk.length     # 5000

# Read ARGV[i] after GC. With the bug, sp_argv.data[i] points to
# freed memory; reading it (and its length) crashes or returns
# garbage.
i = 0
while i < ARGV.length
  puts ARGV[i] + ":" + ARGV[i].length.to_s
  i = i + 1
end
