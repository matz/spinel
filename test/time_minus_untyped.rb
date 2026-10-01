# `t - x` where x may hold a Time or a number: Time - Time is a Float count
# of seconds, Time - a number is a Time. Typed as a Float on the guess that
# x holds a Time (#2456), a number raised TypeError -- activesupport's
# Time#minus_with_coercion passes either to the builtin `-`.
t = Time.at(100).utc
xs = [Time.at(40), 30, 2.5, Rational(1, 2)]
xs.each do |x|
  r = t - x
  p r.class
  p r.is_a?(Time) ? r.to_f : r
end
def since(t, x) = t - x
p since(t, Time.at(99)), since(t, 1).to_i
begin
  t - [Time.at(1), "str"][ARGV.size + 1]
rescue TypeError => e
  p e.class
end
