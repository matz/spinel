# String#upcase, downcase, capitalize, swapcase and downcase(:fold) map case
# over all of Unicode as CRuby does (#11345): the forms of the report, then
# a digest of every codepoint's mappings from U+0080 to U+1FFFF.
s = [0x3A3, 0x391, 0x3A3].pack("U*")
p s.downcase
p "ＡＢＣ".dup.downcase
p "ÀÉ".downcase
p s.downcase(:fold)
p "Straße".downcase(:fold)
p "ﬃ".downcase(:fold)
t = +"Maße"
t.downcase!(:fold)
p t
p :"ΣΑΣ".downcase(:fold)
x = ["Grüße", 3].first
p x.downcase(:fold)
o = :fold
p "ẞ".downcase(o)
begin
  "a".upcase(:fold)
rescue ArgumentError => e
  p e.message
end
p "ǆemal".capitalize
p "ǅ".swapcase
p "ﬁx".upcase
p "İstanbul".downcase
p "straße".casecmp?("STRASSE")
p "ΣΑΣ".casecmp?("σας")
p "ᾀ".capitalize
sums = Hash.new(0)
cnt = Hash.new(0)
cp = 0x80
while cp <= 0x1FFFF
  if cp < 0xD800 || cp > 0xDFFF
    s = [cp].pack("U")
    { u: s.upcase, d: s.downcase, c: s.capitalize, w: s.swapcase, f: s.downcase(:fold) }.each do |k, r|
      next if r == s
      cnt[k] += 1
      r.unpack("U*").each_with_index { |x, i| sums[k] = (sums[k] * 31 + x * (i + 1) + cp) % 1_000_003 }
    end
  end
  cp += 1
end
%i[u d c w f].each { |k| puts "#{k} #{cnt[k]} #{sums[k]}" }
