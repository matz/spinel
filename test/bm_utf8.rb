s = "あいうえお"

puts s.length          # 5
puts s.size            # 5
puts s.bytesize        # 15

puts s.chars.length    # 5
s.chars.each { |c| puts c }

puts s.reverse         # おえういあ

puts s[0]              # あ
puts s[1]              # い
puts s[4]              # お
puts s[-1]             # お
puts s[1, 2]           # いう
puts s[1..3]           # いうえ
puts s.slice(2, 2)     # うえ

puts "hello#{s}world".index(s)   # 5  (char index, not byte)
puts s.index("う")               # 2
puts "あいあい".rindex("い")    # 3

puts "あいう".tr("い", "X")     # あXう
puts "ababab".count("a")        # 3
puts "あいあいあ".count("あ")   # 3
puts "あいあいあ".delete("い")  # あああ
puts "ああいいうう".squeeze     # あいう

puts "あ".ljust(5, "*")          # あ****
puts "あ".rjust(5, "*")          # ****あ
puts "あ".center(5)              # _あ__   (with spaces)
puts "あ".ljust(5).length        # 5   (chars, including padding)

puts "あ".succ                  # ぃ
puts "az".succ                  # ba

puts s.include?("うえ")          # true
puts s.start_with?("あい")       # true
puts s.end_with?("えお")         # true

# each_char with non-ASCII
out = ""
"日本語".each_char { |c| out = out + "[" + c + "]" }
puts out                          # [日][本][語]

# split on empty separator
"あいう".split("").each { |c| puts c }   # あ\nい\nう

# bytes is still byte-based
puts "あ".bytes.length            # 3
