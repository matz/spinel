# Time.at(*args) spreads the arguments into Time.at: seconds, then a
# subsecond part in microseconds or in the unit a third argument names.
# activesupport's TimeZone#at is `Time.at(*args).utc.in_time_zone(self)`.
# The list itself was read as the seconds, and the C did not compile.
def at(*args) = Time.at(*args)

p at(0).utc.to_i
p at(946684800.5).utc.usec
p at(Rational(3, 2)).utc.usec
p at(946684800, 123456.789).nsec
p at(1, 500, :millisecond).utc.usec
p at(1, 7, :nsec).nsec
p at(at(5)).to_i
begin
  at(1, 2, :fortnight)
rescue ArgumentError => e
  p e.message
end
begin
  at("1")
rescue TypeError => e
  p e.message
end
begin
  at
rescue ArgumentError => e
  p e.message
end
