# String#unicode_normalize and its two siblings with no require, as in CRuby:
# NFC, NFD, NFKC (a compatibility character and a halfwidth katakana with its
# voiced mark), an ASCII string, normalized? both ways, the bang form, and an
# unknown form.

p "é".unicode_normalize
p "é".unicode_normalize(:nfd).bytes
p "ｶﾞ①".unicode_normalize(:nfkc)
p "example.com".unicode_normalize(:nfkc)
p "é".unicode_normalized?
p "é".unicode_normalized?
s = +"é"
s.unicode_normalize!
p s
begin
  "a".unicode_normalize(:bogus)
rescue ArgumentError => e
  p e.message
end
