# Time.new takes a splatted Array of fields one by one, as CRuby binds
# them: Time.new(*fields) was read with the whole Array as the year.
def make(*fields)
  Time.new(*fields)
end
p make(2026, 3, 4).month
p make.class
fields = [2026, 5, 6, 7, 8, 9, "+09:00"]
t = Time.new(*fields)
p t.day, t.hour, t.utc_offset
p Time.utc(*[2026, 1, 2]).day
p Time.utc(*Time.utc(2026, 7, 8, 9, 10, 11).to_a).to_s
p Time.local(*[2026, 12, 31]).month
p Time.at(*[0]).to_i
p Time.at(*[1, 500000]).usec
eight = [1] * 8
begin
  Time.new(*eight)
rescue ArgumentError => e
  p e.message
end
nine = [1] * 9
begin
  Time.utc(*nine)
rescue ArgumentError => e
  p e.message
end
none = []
begin
  Time.at(*none)
rescue ArgumentError => e
  p e.message
end
