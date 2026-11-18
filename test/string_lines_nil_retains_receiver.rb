class LineLabel < String
end

label = LineLabel.new("")
line = label.lines(nil, chomp: true).first
p [line.equal?(label), line.class, line.bytes]

def pick(values, index) = values[index]

text = +"a\nb"
answer = pick([text, 1], 0).lines(nil, chomp: true) do |part|
  p part.equal?(text)
  part << "!"
end
p [answer.equal?(text), text]

def record(log, mark, value)
  log << mark
  value
end

log = []
saved = text
answer = record(log, :receiver, text).lines(record(log, :separator, (text = +"new"; nil)),
                                           chomp: record(log, :chomp, true)) do |part|
  log << :yield
  p part.equal?(saved)
end
p [answer.equal?(saved), log, text]

separators = ["|"]
p "a|b".each_line(*separators, chomp: (separators[0] = "/"; true)).to_a
enumerator = "a|b".each_line(*["|", "/"])
p enumerator.class
begin
  enumerator.to_a
rescue ArgumentError => error
  p error.message
end

class LineOverride < String
  def lines = super(nil)
end
line = LineOverride.new("label")
p line.lines.first.equal?(line)

def keyword_value
  puts "keyword evaluated"
  3
end

enumerator = "a|b".each_line("|", bogus: keyword_value)
p enumerator.class
begin
  enumerator.next
rescue ArgumentError => error
  p error.message
end
