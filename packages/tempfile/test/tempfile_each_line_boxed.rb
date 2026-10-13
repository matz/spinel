require "tempfile"

# Tempfile#each_line and #each, with and without a block, with a separator,
# chomp: and a break, and through a value that may also be a String.
file = Tempfile.new("lines")
file.write("ab\ncd\nef")
file.rewind
file.each_line { |line| p line }
file.rewind
file.each_line(chomp: true) { |line| p line }
file.rewind
p file.each_line("d", chomp: true).to_a
file.rewind
p file.each.first
file.rewind
p(file.each_line { |line| break line if line.start_with?("c") })

def first_line(source) = source.each_line(chomp: true).first

file.rewind
p first_line(file)
p first_line("x\ny\n")
file.close!
