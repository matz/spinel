# Integer ** with a literal negative exponent yields a Rational (CRuby
# semantics), typed statically; a run-time exponent that may be negative is
# boxed and answered by its sign (int_pow_runtime_negative_exponent.rb).
# Integer#pow(negative, mod) raises RangeError with CRuby's message.
puts 2 ** 10
puts 0 ** 0
puts 3 ** 4
puts(2.0 ** -1)
p(2 ** -1)        # (1/2)
p(2 ** -2)        # (1/4)
p((-2) ** -2)     # (1/4)
p((-2) ** -3)     # (-1/8)
begin
  p(0 ** -1)
rescue ZeroDivisionError => e
  puts "#{e.class}: #{e.message}"
end
begin
  2.pow(-1, 5)
rescue RangeError => e
  puts "#{e.class}: #{e.message}"
end
