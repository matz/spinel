# ^ Runtime tests use frozen String literals.
def capture_text(s)
  [s.bytes, s.encoding.name]
end

binary = "\xff\x00".b
p capture_text(binary.ljust(7, "x\x00"))
p capture_text("a".rjust(4, "é".b))
p capture_text("a".b.center(4, "é\x00"))
p capture_text("a".center(4, "\xff".b.force_encoding("UTF-8")))
p capture_text("a".b.center(0, "é"))
begin
  binary.ljust(0, "é")
rescue Encoding::CompatibilityError => error
  puts error.message
end

mixed = ("é".b + 255.chr).force_encoding("UTF-8")
p capture_text("a".ljust(2, mixed))
p capture_text("a".rjust(2, mixed))
p capture_text("a".center(2, mixed))
p capture_text(mixed.ljust(3))
p capture_text(mixed.rjust(3))
p capture_text(mixed.center(3))
