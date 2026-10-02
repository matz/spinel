# A program that reaches every part of the regexp engine: compile, match,
# captures, case folding, a UTF-8 subject and a pattern the engine refuses.
# The /i patterns stay ASCII, because the RE_NO_UNICODE_* build refuses a
# non-ASCII one when it compiles the program.
s = "hello world 42"
m = /(\w+) (\d+)/.match(s)
puts m[1] + ":" + m[2]
puts s.gsub(/o/, "0")
puts s.scan(/\d/).join(",")
puts(/(?<word>W\w+)/i.match(s)[:word])
puts "ÀÉÎ wOrLd" =~ /WORLD/i
begin
  Regexp.new("(")
rescue RegexpError
  puts "RegexpError"
end
