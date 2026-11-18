# frozen_string_literal: true
# spinel: gc-stress

# A key made from a byte-producing builtin is one frozen String in the Hash.
# Inverting the table retains that key while constructing the result.
def check_key(table)
  key = table.keys[0]
  inverse = table.invert
  raise "key is mutable" unless key.frozen?
  raise "key changed identity" unless key.equal?(table.keys[0])
  raise "inverse copied key" unless inverse["value"].equal?(key)
end

100.times do
  check_key({"key"[0] => "value"})
  check_key({"key"[0, 0] => "value"})
  check_key({"key".b[0] => "value"})
  check_key({"key".b[0, 0] => "value"})
  check_key({"\0x"[0] => "value"})
  check_key({"\0x".b[0] => "value"})
  check_key({"\xff".b[0] => "value"})
  check_key({"éx"[0] => "value"})
  check_key({:key.name => "value"})
  check_key({+"key" => "value"})
end
puts :ok
