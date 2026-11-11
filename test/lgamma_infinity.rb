# Math.lgamma at +-Infinity: +Infinity gives [Infinity, 1] (the series used to
# compute inf - inf and answer NaN); -Infinity is out of domain and raises.
p Math.lgamma(Float::INFINITY)
begin
  Math.lgamma(-Float::INFINITY)
rescue Math::DomainError => e
  puts "#{e.class}: #{e.message}"
end
p Math.lgamma(Float::NAN)
p Math.lgamma(1.0e308)
p Math.lgamma(-0.0)
