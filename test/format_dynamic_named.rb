# format/sprintf: dynamic width/precision (%*d, %.*f) consume the next
# positional argument; %<name>conv and %{name} reference a hash argument by
# key (missing keys raise KeyError). Unknown specs used to echo verbatim.
puts format("%*d", 5, 3)
puts format("%-*d|", 5, 3)
puts format("%*d", -5, 3) + "|"
puts format("%.*f", 2, 3.14159)
puts format("%08.*f", 3, 3.14159)
puts format("%<val>d", val: 42)
puts format("%<val>05d", val: 42)
puts format("%{val}", val: 42)
puts format("%{a}-%{b}", a: "x", b: 9)
puts sprintf("%<f>.2f", f: 1.2345)
begin
  format("%<nope>d", val: 1)
rescue KeyError => e
  puts "KeyError"
end
begin
  format("%{name}", {"name" => "string"})
rescue KeyError => e
  p [e.message, e.key]
end
puts format("%{name}", {"name" => "string", :name => "symbol"})
h = {"name" => "string", "other" => 2}
h.default = "fallback"
puts format("%{name}", h)
puts "%{name}" % h
h = Hash.new { |owner, key| key.inspect }
h[:present] = nil
p format("%{missing}", h), format("%{present}", h)
typed = {"name" => "string", "other" => 2}
typed.default_proc = proc { |owner, key| key.class.to_s }
p format("%{name}", typed)
