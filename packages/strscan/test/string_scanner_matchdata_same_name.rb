# A boxed MatchData answers its own post_match / pre_match beside
# StringScanner's methods of the same names.
require "strscan"

def subject(kind)
  case kind
  when 0 then /b/.match("abc")
  when 1 then StringScanner.new("xy").tap { |s| s.scan(/x/) }
  when 2 then 42
  end
end

puts subject(0).post_match
puts subject(1).post_match
puts subject(0).pre_match
puts subject(1).pre_match
begin
  subject(2).post_match
rescue NoMethodError => e
  puts e.message
end
