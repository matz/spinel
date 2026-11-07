# Print only integers and booleans so embedded NUL never reaches stdout.
samples = ["", "\0", "a\0b", "\0ab", "ab\0", "é\0🙂", "\0é🙂", "é🙂\0"]
samples.each do |text|
  points = text.codepoints
  puts points.length
  puts points
  walked = []
  result = text.each_codepoint { |point| walked << point }
  puts points == walked
  puts result == text
  puts text.each_codepoint.to_a == walked
  puts text.bytes.length == text.bytesize
end
