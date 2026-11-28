# URI.encode_www_form_component escapes byte by byte: every byte but
# A-Z a-z 0-9 * - . _ as %XX in capitals, a space as "+", a multibyte
# character as each of its bytes, and a non-String through to_s.
require "uri"

all = (0..255).map(&:chr).join.b
puts URI.encode_www_form_component(all)
puts URI.encode_www_form_component("café 日本 ~!'()")
puts URI.encode_www_form_component("a b+c&d=e/f?g")
puts URI.encode_www_form_component("\xff\xfe broken".b)
puts URI.encode_www_form_component(:sym_bol)
puts URI.encode_www_form_component(42)
p URI.encode_www_form_component(nil)
p URI.encode_www_form_component("")
p URI.encode_uri_component("a b")
p URI.encode_www_form([["q", "x y"], ["k", "é"]])
