# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
r = [Rational(7, 3)][0]

[:finite?, :infinite?].each do |name|
  begin
    value = case name
    when :finite? then r.finite?
    when :infinite? then r.infinite?
    end
    p [name, value.class, value]
  rescue => error
    p [name, error.class, error.message]
  end
end

[Rational(0, 1), Rational(-7, 3), Rational(2**100, 3), Rational(1, 2**1200)].each do |value|
  p [[value, "x"][0].finite?, [value, "x"][0].infinite?]
end

def read_rational(events)
  events << :receiver
  GC.start
  [Rational(7, 3), "x"][0]
end
events = []
p read_rational(events).finite?
p read_rational(events).infinite?
p events

[nil, "invalid"].each do |value|
  begin
    p [value, 1][0].finite?
  rescue => error
    p [error.class, error.message]
  end
end
p Rational(7, 3).finite?, Rational(7, 3).infinite?
p [2.0, "mixed"][0].finite?, [2.0, "mixed"][0].infinite?
p [1.0 / 0, "mixed"][0].finite?, [-1.0 / 0, "mixed"][0].infinite?
