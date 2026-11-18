# spinel: gc-minor
# String line traversal selects the text before converting its separator.
class LineSeparator
  def initialize(source)
    @source = source
  end

  def to_str
    @source.replace("x|y")
    GC.start
    "|"
  end
end

def separator(values)
  values[0]
end

[+"a|b", +""].each do |source|
  sep = separator([LineSeparator.new(source), "|"])
  p [source.lines(sep), source]
end

text = +"a|b"
sep = LineSeparator.new(text)
walk = text.each_line(sep)
text.replace("c|d")
p [walk.next, walk.next, text]

blank = +""
walk = blank.each_line(LineSeparator.new(blank))
begin
  walk.next
rescue StopIteration
  p [:done, blank]
end

begin
  "".each_line(42).next
rescue StopIteration
  p :done
end
