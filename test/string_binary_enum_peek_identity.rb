# spinel: gc-minor
# A peeked character keeps its identity and encoding when next returns it.
text = [+"\xc3\xa9\x00".b, 1][0]
characters = text.each_char
head = characters.peek
p [head.encoding.name, head.bytes]
head << "!" * 256
out = characters.next
p [out.equal?(head), out.bytesize]
p [characters.next.encoding.name, characters.next.bytes, text.bytesize]
