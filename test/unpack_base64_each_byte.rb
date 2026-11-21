# Each of the 256 byte values in a base64 quad: m0 decodes the 64 alphabet
# bytes to their sextet and raises for the others, and m skips the others
# except '=', which ends the data.
strict = []
invalid = 0
256.times do |b|
  quad = "AA" + b.chr.b + "A"
  begin
    s = quad.unpack1("m0")
    strict << (s.getbyte(1) & 0x0f) * 4 + (s.getbyte(2) >> 6)
  rescue ArgumentError
    invalid += 1
  end
end
p strict
p invalid
lenient = 0
256.times do |b|
  lenient += 1 if ("QU" + b.chr.b + "JD").unpack1("m") == "ABC"
end
p lenient
