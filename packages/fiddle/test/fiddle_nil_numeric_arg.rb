# nil where a Fiddle function or closure wants a C integer or double is a
# TypeError, as CRuby's NUM2INT / NUM2DBL raise; it was passed as 0 / 0.0.
require "fiddle"

h = Fiddle::Handle::DEFAULT
abs = Fiddle::Function.new(h["abs"], [Fiddle::TYPE_INT], Fiddle::TYPE_INT)
p abs.call(-3)
begin
  p abs.call(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
fabs = Fiddle::Function.new(Fiddle.dlopen(nil)["fabs"], [Fiddle::TYPE_DOUBLE], Fiddle::TYPE_DOUBLE)
p fabs.call(-2.5)
begin
  p fabs.call(nil)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
