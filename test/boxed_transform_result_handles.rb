def grow_stored_result(held)
  value = held[0]
  p [value.bytes, value.encoding.name, value.frozen?]
  value << "x" * 111
  p [value.bytesize, held[0].bytesize]
end

def byte_slice(value, start, length)
  value.byteslice(start, length)
end

def range_slice(value)
  value.byteslice(1..2)
end

def justify(value)
  value.ljust(6)
end

def clean(value)
  value.scrub("*")
end

def transcode(value)
  value.encode("UTF-8")
end

def copy_encoding(value)
  value.encode
end

source = ["ab\0c".b, 7][0]
grow_stored_result([byte_slice(source, 1, 2), 1])
grow_stored_result([byte_slice(source, 4, 0), 1])
grow_stored_result([range_slice(source), 1])
grow_stored_result([justify(source), 1])
grow_stored_result([clean(source), 1])
grow_stored_result([transcode(source), 1])
grow_stored_result([copy_encoding(source), 1])
p [source.bytes, byte_slice(source, 5, 1)]
