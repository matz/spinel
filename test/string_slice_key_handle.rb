def boxed_slice(source, key)
  source[key]
end

source = "a\0b".b
key = "\0b".b.freeze
missing = "\0z".b
p [source["b".b].bytes, source.slice(key).bytes, source[missing], source.slice(missing)]

boxed = [source, 83][0]
first = boxed_slice(boxed, key)
second = boxed_slice(boxed, key)
held = [first, 3]
p [first.equal?(key), first.equal?(second), first.encoding.name, first.bytes, first.frozen?]
first << "x" * 175
p [held[0].bytesize, second.bytesize, key.bytesize, source.bytesize]
p [boxed_slice(boxed, "a").frozen?, boxed_slice(boxed, "z")]

text = "é"
p [text["".b].encoding.name, text["\xA9"]]
begin
  text.slice(text.b)
rescue Encoding::CompatibilityError => error
  p error.class
end
