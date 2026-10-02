# Read-only Hash value blocks and a frozen literal keep their behaviour.
s = +"a"
h = {k: s}
h.each_value { |q| p q + "!" }
h.each { |k, q| p k, q }
h.each_pair { |k, q| p q }
h.values.each { |q| p q }
p s, h
h6 = { k: "lit" }
begin
  h6.each_value { |q| q << "x" }
rescue => e
  p e.class
end

# Store accepts an ignored block; reading the value is still supported.
stored = +"stored"
hs = {}
hs.store(:k, stored) { raise "not called" }
hs.each_value { |v| p v }
hs.each_pair { |k, v| p k, v }
p stored
