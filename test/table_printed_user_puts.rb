# A program with a puts of its own, at the top or in a class: a table
# handed to it is the program's to keep, and stays a general Array (see
# table_printed_narrows.rb).
def puts(*a) = $stdout.write("puts of #{a.size}: #{a.first.inspect}\n")
x = [[1, 2], [3, 4]]
puts x
x[0] << 5
p x[0].size

class Report
  def puts(t) = $stdout.write("Report#puts #{t.size}\n")
  def run
    rows = [[1], [2, 3]]
    puts rows
    rows[0] << 4
    p rows[0]
  end
end
Report.new.run
