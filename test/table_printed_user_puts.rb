# A program with a puts of its own: a table handed to it is the program's
# to keep, and stays a general Array (see table_printed_narrows.rb).
def puts(*a) = $stdout.write("puts of #{a.size}: #{a.first.inspect}\n")
x = [[1, 2], [3, 4]]
puts x
x[0] << 5
p x[0].size
