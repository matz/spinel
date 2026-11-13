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
r = URI.decode_www_form_component(s)
p r.bytesize
p r.end_with?("aA ")

# The time to decode grows with the length, not with its square: eight
# times the text takes about eight times as long, where a decoder that
# counts characters from the start for each one took sixty-four. Both runs
# are timed here, so the ratio does not depend on how fast the machine is;
# each is the best of five, so one pause does not decide it.
def best_ms(s)
  (1..5).map {
    t = Time.now.to_f
    URI.decode_www_form_component(s)
    (Time.now.to_f - t) * 1000
  }.min
end
short = best_ms("é" + ("a" * 25_000))
long = best_ms("é" + ("a" * 200_000))
ratio = long / (short > 0.01 ? short : 0.01)
puts(ratio < 24 ? "linear" : "quadratic: #{short.round(2)} ms -> #{long.round(2)} ms")
