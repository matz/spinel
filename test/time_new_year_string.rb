# Time.new with a String that is a year alone reads it as Time.new(year):
# four digits at least, a sign allowed; fewer is CRuby's ArgumentError.
%w[2021 12 0021 20210 123 1999 -2021 +2021 99999].each do |s|
  t = Time.new(s)
  p [t.year, t.month, t.day, t.hour, t.min]
rescue ArgumentError => e
  p [e.class, e.message]
end
p Time.new("2021").utc_offset == Time.new(2021).utc_offset
p Time.new("2021-03-04 05:06:07").month
