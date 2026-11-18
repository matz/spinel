h = {"x" => 1, "y" => "two"}
callback = ->(hash, key) { [hash, key] }
h.default_proc = callback
reader = h.default_proc
p reader.equal?(callback)
p reader.call({}, Encoding::UTF_8)
key = +"value"
result = reader.call(h, key)
p [result[0].equal?(h), result[1].equal?(key)]
begin
  reader.call(h)
rescue ArgumentError => error
  p error.message
end
