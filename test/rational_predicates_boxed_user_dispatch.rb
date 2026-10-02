# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
# A user class that owns the names keeps its methods; boxed Rationals beside it answer.
class QueryOwner
  def finite?
    false
  end
  def infinite?
    1
  end
end
values = [Rational(7, 3), Rational(0, 1), Rational(1, 2**1200), QueryOwner.new]
values.each do |value|
  begin
    p [value.finite?, value.infinite?]
  rescue => error
    p [error.class, error.message]
  end
end
