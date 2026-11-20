def select_string(value, present)
  puts "receiver"
  present ? value : nil
end

def pattern(value)
  puts "pattern"
  value.replace("aba")
  "a"
end

def make_block(value)
  puts "block"
  value.replace("aaba")
  GC.start
  ->(match) { puts match; "G" }
end

word = +"a"
p select_string(word, false)&.gsub(pattern(word), &make_block(word))
p word
p select_string(word, true)&.gsub(pattern(word), &make_block(word))
p word
