# spinel: gc-minor
# Build each adjacent literal before evaluating the next one.
SegmentCell = Struct.new(:v)

def pass_segment(value) = value

s = +"a"
cell = SegmentCell.new
text = ("#{(cell.v = s) << "b"}|#{s << "c"}" "#{pass_segment(s.replace("d"))}")
p [text, s]

ascii = +"a".b
text = ("#{(cell.v = ascii) << "x"}" "#{ascii << "y"}")
p [text.bytes, text.encoding.name, ascii.encoding.name]
p ["#{ascii}".encoding.name, "#{ascii}#{ascii}".encoding.name]
p ["#{ascii}#{}".encoding.name, "#{}#{ascii}".encoding.name]

bad = +"\xff".b
p "#{bad}".encoding.name
utf8 = +"é"
begin
  ("#{(cell.v = bad) << "x"}|#{utf8 << "z"}" "#{puts :later}")
rescue Encoding::CompatibilityError
  puts :encoding_error
end
