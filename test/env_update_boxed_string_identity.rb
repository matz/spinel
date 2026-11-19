# spinel: gc-minor
# The incoming value reaches the conflict block before ENV converts it.
key = "SPINEL_UPDATE_IDENTITY"
$incoming = +"new"
ENV[key] = "old"
h = {key => $incoming, "SPINEL_UPDATE_UNUSED" => nil}
$stored_key = h.keys[0]
ENV.update(h) do |k, old, value|
  p [k.equal?($stored_key), k.frozen?, old.frozen?, value.equal?($incoming)]
  value << "!"
  GC.start
  value
end
p [ENV[key], $incoming]
