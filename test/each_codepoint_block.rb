# String#each_codepoint with a block compiled into a NoMethodError at run
# time, while the blockless form and `codepoints { }` worked (#7198). A
# typed String's each_codepoint with a block is now the codepoints walk,
# yielding each character's ordinal and answering the receiver, as CRuby's
# does. did_you_mean's Levenshtein walks a word this way.
"ab".each_codepoint { |c| p c }

word = "héllo"
word.each_codepoint { |c| print c, " " }
puts

def ords(s)
  out = []
  s.each_codepoint { |c| out << c }
  out
end
p ords("日本")

p "xy".each_codepoint { |c| c }
p "abc".each_codepoint.with_index(1).to_a
p "abc".each_codepoint.to_a
p "héllo".codepoints
