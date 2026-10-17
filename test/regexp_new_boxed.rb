# Regexp.new of a value read out of a Hash (boxed), diffed against CRuby:
# a Regexp is copied with its options (a second argument ignored, as CRuby
# ignores it), a String compiles, anything else is TypeError. The boxed
# argument read as a String throughout, and a Regexp raised TypeError.
t = { re: /a+b/xi, s: "c+d", n: 1 }
r = Regexp.new(t[:re])
p [r.source, r.options, r.match?("AAB")]
r2 = Regexp.new(t[:s])
p [r2.source, r2.match?("ccd")]
r3 = Regexp.new(t[:s], Regexp::IGNORECASE)
p [r3.source, r3.options, r3.match?("CCD")]
r4 = Regexp.new(t[:re], Regexp::MULTILINE)
p r4.options
begin
  Regexp.new(t[:n])
rescue TypeError => e
  p e.message
end
