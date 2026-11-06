# Time.new(string, in: zone): the keyword's zone reads a string that carries
# none, and a zone the string carries wins.
puts Time.new("2026-11-04 10:20:30", in: "UTC")
puts Time.new("2026-11-04 10:20:30", in: "+05:00")
puts Time.new("2026-11-04 10:20:30", in: 3600)
puts Time.new("2026-11-04 10:20:30 +09:00", in: "UTC")
puts Time.new("2026-11-04 10:20:30.5", in: "Z").subsec
puts Time.new("2021", in: "UTC")
zone = ARGV.empty? ? "-02:00" : 7
puts Time.new("2026-11-04 10:20:30", in: zone)
puts Time.new("2026-11-04 10:20:30 +09:00", in: "invalid")
begin
  Time.new("2026-11-04 10:20:30", in: "invalid")
rescue ArgumentError
  puts "ArgumentError"
end
