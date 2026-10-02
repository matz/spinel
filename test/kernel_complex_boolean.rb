# A Boolean is not a Complex scalar conversion or a real component.
begin
  p Complex(false)
rescue => error
  p [error.class, error.message]
end
[false, true].each do |value|
  begin
    p Complex(value)
  rescue => error
    p [error.class, error.message]
  end
  begin
    p Complex(1, value)
  rescue => error
    p [error.class, error.message]
  end
  begin
    p Complex(value, 1)
  rescue => error
    p [error.class, error.message]
  end
end
# Boxed Booleans give the same errors, while numbers still construct.
[false, true, 2, 2.0].each do |value|
  begin
    p Complex(value)
  rescue => error
    p [error.class, error.message]
  end
  begin
    p Complex(1, value)
  rescue => error
    p [error.class, error.message]
  end
end
# Both arguments run once, in order, before the error.
$events = []
def bad_real
  $events << "real"
  false
end
def imag_effect
  $events << "imaginary"
  GC.start
  1
end
begin
  Complex(bad_real, imag_effect)
rescue => error
  p [error.class, error.message]
end
p $events
# A String is parsed before the real-number check, and nil comes first.
begin
  Complex("bad", false)
rescue ArgumentError
  puts "String parsed first"
end
begin
  Complex(false, "2")
rescue => error
  p [error.class, error.message]
end
begin
  Complex(true, Complex(1, 2))
rescue => error
  p [error.class, error.message]
end
begin
  Complex(true, {a: 1})
rescue => error
  p [error.class, error.message]
end
class Part
end
begin
  Complex(Part.new, false)
rescue => error
  p [error.class, error.message]
end
begin
  Complex(false, nil)
rescue => error
  p [error.class, error.message]
end
begin
  Complex(true, [nil, 1][0])
rescue => error
  p [error.class, error.message]
end
# A heap String stays live while the Boolean argument runs GC.
def gc_boolean
  GC.start
  false
end
begin
  Complex(+"2", gc_boolean)
rescue => error
  p [error.class, error.message]
end
p Complex(2, 3)
