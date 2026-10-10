# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
# center/ljust/rjust, with and without a pad, answer a width past the
# string size limit as CRuby does: NoMemoryError, or ArgumentError for a
# byte total past a long. They used to ask the allocator for it, and a
# failed allocation ends the process (sp_oom_die), so one caller-chosen
# width could stop a whole program that rescues the error. The padded forms
# first walked the width a character at a time, so a width like 1 << 62 ran
# for minutes before getting that far.
begin
  "ab".center(1 << 40)
rescue ArgumentError, NoMemoryError => e
  puts "center 2^40: #{e.class}: #{e.message}"
end
begin
  "ab".ljust(9223372036854775807)
rescue ArgumentError, NoMemoryError => e
  puts "ljust max: #{e.class}: #{e.message}"
end
begin
  "ab".rjust(1 << 40)
rescue ArgumentError, NoMemoryError => e
  puts "rjust 2^40: #{e.class}: #{e.message}"
end
begin
  "ab".center(1 << 62, "xy")
rescue ArgumentError, NoMemoryError => e
  puts "center pad 2^62: #{e.class}: #{e.message}"
end
begin
  "ab".ljust((1 << 61) + 1, "é")
rescue ArgumentError, NoMemoryError => e
  puts "ljust multibyte pad: #{e.class}: #{e.message}"
end
begin
  "ab".rjust(1 << 40, "-")
rescue ArgumentError, NoMemoryError => e
  puts "rjust pad 2^40: #{e.class}: #{e.message}"
end
begin
  "ab".center(10, "")
rescue ArgumentError, NoMemoryError => e
  puts "empty pad: #{e.class}: #{e.message}"
end
puts "ab".center(8, "*")
puts "ab".ljust(5, "é")
puts "ab".rjust(5)
puts "ab".center(1).inspect
puts "still running"
