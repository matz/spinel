# A NUL field is not empty, including when trailing empty fields are removed.
samples = ["a\0b", "\0ab", "ab\0", "é\0🙂", "\0é🙂", "é🙂\0", "\0", "", "é\0\0🙂"]
samples.each do |text|
  puts text.split(//) == text.chars
  puts text.split(//, 0) == text.chars
  puts text.split(//, 1) == (text.empty? ? [] : [text])
  puts text.split(//, 2).join == text
  puts text.split(//, -1).join == text
  puts text.split("") == text.chars
end
puts "a,\0b".split(/,/) == ["a", "\0b"]
puts "a,\0".split(/,/) == ["a", "\0"]
puts "a,\0,,".split(/,/) == ["a", "\0"]
puts "a,\0,,".split(/,/, -1) == ["a", "\0", "", ""]
puts "a,\0b".split(/(,)/) == ["a", ",", "\0b"]
puts "é,\0🙂".split(/,/) == ["é", "\0🙂"]
puts "é,\0🙂".b.split(/,/) == ["é".b, "\0🙂".b]
