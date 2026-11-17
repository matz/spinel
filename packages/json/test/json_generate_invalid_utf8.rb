require "json"
# A String whose bytes are not valid UTF-8 is refused with JSON::GeneratorError,
# as CRuby's json refuses it, rather than written into the JSON text as raw
# bytes no parser reads back. A binary String gets the message of its failed
# conversion to UTF-8, which names its first byte past ASCII; a binary String
# whose bytes are UTF-8 is still taken.

def gen(label)
  puts "#{label}: #{yield}"
rescue JSON::GeneratorError => e
  puts "#{label}: #{e.class}: #{e.message}"
end

# text Strings: a stray byte, a cut sequence, a surrogate half, an overlong
# form, a code point past U+10FFFF
gen("stray") { JSON.generate(["a\xFFb"]) }
gen("cut") { JSON.generate(["a\xE3\x81"]) }
gen("surrogate") { JSON.generate(["\xED\xA0\x80"]) }
gen("overlong") { JSON.generate(["\xC0\xAF"]) }
gen("past max") { JSON.generate(["\xF4\x90\x80\x80"]) }
gen("bad continuation") { JSON.generate(["\xE3\x41\x81"]) }
gen("cut at a lead") { JSON.generate(["日\xE3"]) }

# binary Strings
gen("binary") { JSON.generate([[65, 255, 66].pack("C*")]) }
gen("binary cut") { JSON.generate([[0x61, 0xE3, 0x81, 0x62].pack("C*")]) }
gen("binary after utf8") { JSON.generate(["é\xFF".b]) }
gen("binary utf8") { JSON.generate(["é".b]) }
gen("binary ascii") { JSON.generate(["abc".b]) }

# valid UTF-8 is written as it is, next to escapes
gen("valid") { JSON.generate(["héllo", "日本", "\u{1F600}", "\"é\n\""]) }
# the first and last character of each length, and next to a surrogate half
gen("edges") { JSON.generate(["\u0080߿ࠀ퟿￿\u{10000}\u{10FFFF}"]).bytes.sum }

# a key, a nested value, to_json, pretty_generate, dump
gen("key") { JSON.generate({[0xFF].pack("C") => 1}) }
gen("text key") { JSON.generate({"k\xC3" => 1}) }
gen("nested") { JSON.generate({"a" => [1, {"b" => "x\xFF"}]}) }
gen("to_json") { "a\xFF".to_json }
gen("binary to_json") { [0xFF].pack("C").to_json }
gen("pretty") { JSON.pretty_generate(["a\xFF"]) }
gen("dump") { JSON.dump(["a\xFF"]) }

# a String built at run time, one changed in place, and one past the
# generator's stack buffer
s = "ok" + [0x80].pack("C").force_encoding("UTF-8")
gen("built") { JSON.generate([s]) }
m = +"abc"
m << "\xFE"
gen("mutated") { JSON.generate([m]) }
long = "x" * 2000
gen("long") { JSON.generate([long, long + "\xFF"]) }
# pretty_generate past its stack buffer, with leaves of every kind: the
# refusal comes from the walk's own buffer, and the next call works
mixed = Array.new(60) { |i| ["s#{i}", i, i * 1.5, :"y#{i}", nil, true, 2**70] }.flatten
gen("pretty long") { JSON.pretty_generate(mixed + ["é\xFF"]) }
gen("pretty long nan") { JSON.pretty_generate(mixed + [Float::NAN]) }
gen("pretty long ok") { JSON.pretty_generate(mixed + ["é"]).bytesize }

# a #to_json of the program's own is its own text, written as it is
class RawDocument
  def to_json(*args) = "\"a\xFFb\""
end
class RawObject
  def to_json(*args) = "{\"k\xFF\":[\"v\xFE\"]}"
end
gen("user to_json") { JSON.generate([RawDocument.new]).bytes.inspect }
gen("user to_json pretty") { JSON.pretty_generate([RawDocument.new]).bytes.inspect }
gen("user to_json object") { JSON.generate({"a" => RawObject.new}).bytes.inspect }

# the generator still works after a refusal
gen("after") { JSON.generate(["fine", "é"]) }
