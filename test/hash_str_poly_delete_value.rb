# spinel: gc-stress
# String-keyed mixed-value hashes: deletion returns the stored value, not the
# default, even for nil/false, and keeps object identity and insertion order.
values = [1, 2]
h = { "integer" => 7, "text" => +"value", "nil" => nil,
      "false" => false, "array" => values }
p h.delete("integer"), h.delete("text"), h.delete("nil"), h.delete("false")
p h.key?("nil"), h.key?("false"), h.size
removed = h.delete("array")
GC.start
p removed.equal?(values), removed, h.empty?, h.delete("absent")

with_default = Hash.new("default")
with_default["integer"] = 7
with_default["text"] = "value"
p with_default.delete("absent"), with_default["absent"]
p with_default.delete("integer"), with_default.keys

calls = []
with_proc = Hash.new { |_hash, key| calls << key; "default" }
with_proc["integer"] = 7
with_proc["text"] = "value"
p with_proc.delete("absent"), calls
p with_proc["absent"], calls

# Equal bytes need not be the same String object; embedded NUL and binary keys
# must still use the complete byte sequence.
key = +"mutable"
bytes = "a\0b"
binary = [0, 255, 128].pack("C*")
strings = { key => 1, bytes => "nul", binary => false }
key << " changed"
p strings.delete("mutable".dup), strings.delete(bytes.dup)
p strings.delete(binary.dup), strings.empty?

ordered = { "first" => 1, "middle" => "two", "last" => nil }
p ordered.delete("middle")
ordered["middle"] = false
p ordered.keys, ordered.values

# The specialized no-block path must not affect missing-key block behavior.
p ordered.delete("last") { "not called" }
p ordered.delete("absent") { |missing| "missing #{missing}" }
p ordered.keys

frozen_hash = { "integer" => 7, "text" => "value" }.freeze
["integer", "absent"].each do |name|
  begin
    frozen_hash.delete(name)
  rescue FrozenError => error
    p error.class, error.receiver.equal?(frozen_hash)
  end
end
p frozen_hash.keys
