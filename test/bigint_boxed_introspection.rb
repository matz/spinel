b = [2 ** 100, "x"][0]

[:bit_length, :size, :numerator, :denominator, :nonzero?, :digits].each do |name|
  begin
    value = case name
    when :bit_length then b.bit_length
    when :size then b.size
    when :numerator then b.numerator
    when :denominator then b.denominator
    when :nonzero? then b.nonzero?
    when :digits then b.digits
    end
    p [name, value.class, name == :digits ? value.length : value]
  rescue => error
    p [name, error.class, error.message]
  end
end
p [b.numerator.equal?(b), b.nonzero?.equal?(b)]
[0, 7, -7, "skip"].each do |value|
  next if value.is_a?(String)
  p [value.numerator, value.denominator, value.nonzero?]
end
class Custom
  def numerator; :custom; end
  def denominator; :custom; end
  def nonzero?; :custom; end
end
[Custom.new, b].each do |value|
  p [value.numerator, value.denominator, value.nonzero?]
end
# nonzero? on the other numeric kinds read from a mixed Array.
[0.0, 1.5, Rational(1, 2), Complex(1, 2), Complex(0, 0)].each do |n|
  p [n, "x"][0].nonzero?
end
