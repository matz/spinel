# URI's percent decoding walks the text by byte, so a raw non-ASCII
# character does not change the result. Reading it by character made the
# decode quadratic in the length (4.7 s for the 400,000-byte value below);
# that only shows as time, so what the test holds is the result.
require "uri"

p URI.decode_www_form_component("caf%C3%A9+%E2%82%AC")
p URI.decode_www_form_component("café+crème%21")
p URI.decode_uri_component("é+%C3%A9")
p URI.decode_www_form("k=%E5%80%A4+v&a=%zz+%")
p URI.decode_www_form_component("é%C3").valid_encoding?
p URI.decode_www_form_component("é").encoding
p (URI.decode_www_form_component("é%zz") rescue [$!.class, $!.message])

s = "é" + ("a" * 400_000) + "%41+"
r = URI.decode_www_form_component(s)
p r.bytesize
p r.end_with?("aA ")
