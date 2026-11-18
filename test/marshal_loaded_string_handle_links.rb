# spinel: gc-minor
# Marshal links keep one mutable String across the decoded graph.
text = +"é\0!"
binary = +"\xff\0".b
copy = Marshal.load(Marshal.dump([text, text, {text: text}, text.dup, binary, binary]))
first = copy[0]
raw = copy[4]
p [first.equal?(copy[1]), first.equal?(copy[2][:text]), first.equal?(copy[3])]
p [first.encoding.name, first.bytes, raw.encoding.name, raw.bytes]
first << "a" * 300
raw << "b".b * 300
p [copy[0].bytesize, copy[1].bytesize, copy[2][:text].bytesize, copy[3].bytesize]
p [copy[4].equal?(copy[5]), copy[5].bytesize, copy[5].encoding.name]
p [text.bytesize, binary.bytesize]

["\x04\x08\"\x08a".b, "\x04\x08\"\xfa".b].each do |bad|
  begin
    Marshal.load(bad)
  rescue ArgumentError => e
    p e.message
  end
end
