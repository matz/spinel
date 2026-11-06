# A method that returns early and otherwise ends in its yield answers the
# early value when the block given never hands one back: one that raises,
# or one that ends in a puts.

def name_or(c)
  return c.to_s if /\A[a-z_0-9]+\z/.match?(c)
  yield
end

def check(part)
  yield(part)
end

p name_or("hello") { raise ArgumentError, "bad" }
p check("posts") { |part| name_or(part) { raise ArgumentError, "bad #{part}" } }
begin
  name_or("Bad!") { raise ArgumentError, "bad name" }
rescue ArgumentError => e
  p e.message
end

x = name_or("world") { puts "block" }
p x
y = name_or("Bad!") { puts "block" }
p y
