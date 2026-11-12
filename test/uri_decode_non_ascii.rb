# URI's percent decoding walks the text by byte, so a raw non-ASCII
# character neither changes the result nor makes the time grow with the
# square of the length.
require "uri"

p URI.decode_www_form_component("caf%C3%A9+%E2%82%AC")
p URI.decode_www_form_component("café+crème%21")
p URI.decode_uri_component("é+%C3%A9")
p URI.decode_www_form("k=%E5%80%A4+v&a=%zz+%")
p URI.decode_www_form_component("é%C3").valid_encoding?
p URI.decode_www_form_component("é").encoding
p (URI.decode_www_form_component("é%zz") rescue [$!.class, $!.message])

s = "é" + ("a" * 400_000) + "%41+"
t = Time.now.to_f
r = URI.decode_www_form_component(s)
ms = (Time.now.to_f - t) * 1000
p r.bytesize
p r.end_with?("aA ")
puts(ms < 500 ? "linear" : "slow: #{ms.round} ms")
