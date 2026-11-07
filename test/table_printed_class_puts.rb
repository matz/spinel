# A class with a puts of its own and no top-level one: a bare puts inside
# the class calls it, not Kernel#puts, and the table it is handed is that
# method's argument (see table_printed_narrows.rb).
class Report
  def puts(t) = $stdout.write("Report#puts #{t.size} #{t.inspect}\n")
  def build = Array.new(2) { |i| [i, i * 2] }

  def run
    puts build
    rows = build
    puts rows
    rows[0] << 9
    p rows[0]
  end
end
Report.new.run
puts [[1, 2], [3]]
