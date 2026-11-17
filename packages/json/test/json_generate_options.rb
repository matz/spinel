require "json"
# JSON.generate's options are read as CRuby's json reads them: ascii_only,
# script_safe, allow_nan and max_nesting change the answer, and so they do
# for pretty_generate and a builtin's #to_json; a name CRuby does not know
# raises "unknown keyword", where the options used to be dropped unread.

def gen(label)
  puts "#{label}: #{yield}"
rescue ArgumentError, TypeError, JSON::GeneratorError, JSON::NestingError => e
  puts "#{label}: #{e.class}: #{e.message}"
end

# ascii_only: every character past ASCII as \uXXXX, a surrogate pair past
# U+FFFF; a value, a key, a Symbol, a key made from #to_s, a binary String
gen("ascii") { JSON.generate(["é日\u{1F600}\u0001\u007f", "plain"], ascii_only: true) }
gen("ascii key") { JSON.generate({"é" => 1, :"ü" => 2, 1.5 => "é"}, ascii_only: true) }
gen("ascii sym") { JSON.generate([:"é"], ascii_only: true) }
gen("ascii binary") { JSON.generate(["é".b], ascii_only: true) }
gen("ascii scalar") { JSON.generate("é", ascii_only: true) }
gen("ascii false") { JSON.generate(["é"], ascii_only: false) }
gen("ascii invalid") { JSON.generate(["\xFF"], ascii_only: true) }

# script_safe: "/", U+2028 and U+2029 escaped; with ascii_only too
gen("script") { JSON.generate(["</script>  é"], script_safe: true) }
gen("script key") { JSON.generate({"a/b" => " "}, script_safe: true) }
gen("both") { JSON.generate(["/ é"], script_safe: true, ascii_only: true) }

# allow_nan
gen("nan") { JSON.generate([Float::NAN, Float::INFINITY, -Float::INFINITY], allow_nan: true) }
gen("nan scalar") { JSON.generate(Float::NAN, allow_nan: true) }
gen("nan false") { JSON.generate([Float::NAN], allow_nan: false) }

# max_nesting: N levels pass, the next raises; 0 and nil are none; a
# negative one refuses the first container
gen("nest 3") { JSON.generate([[[1]]], max_nesting: 3) }
gen("nest 2") { JSON.generate([[[1]]], max_nesting: 2) }
gen("nest hash") { JSON.generate({"a" => {"b" => 1}}, max_nesting: 1) }
gen("nest 0") { JSON.generate([[[[1]]]], max_nesting: 0) }
gen("nest nil") { JSON.generate([[[[1]]]], max_nesting: nil) }
gen("nest -1") { JSON.generate([1], max_nesting: -1) }
gen("nest float") { JSON.generate([[1]], max_nesting: 1.9) }
gen("nest str") { JSON.generate([1], max_nesting: "x") }
deep = [1]
150.times { deep = [deep] }
gen("nest 200") { JSON.generate(deep, max_nesting: 200).bytesize }

# options that change nothing here, or are the defaults
gen("defaults") { JSON.generate({"a" => [1]}, indent: "", space: nil, object_nl: false, array_nl: "", space_before: "") }
gen("noop") { JSON.generate([1], buffer_initial_length: 64, depth: 0, strict: false, sort_keys: false, as_json: nil) }
gen("none") { JSON.generate([1], {}) }
gen("nil") { JSON.generate(["é"], nil) }
gen("false") { JSON.generate(["é"], false) }

# the options as a value, and splatted
opts = {ascii_only: true, script_safe: true}
gen("hash value") { JSON.generate(["/é"], opts) }
gen("splat") { JSON.generate(["/é"], **opts) }

# what CRuby refuses
gen("unknown") { JSON.generate([1], foo: 1) }
gen("unknowns") { JSON.generate([1], foo: 1, bar: 2) }
gen("unknown and known") { JSON.generate(["é"], foo: 1, ascii_only: true) }
gen("string key") { JSON.generate([1], {"ascii_only" => true}) }
gen("not a hash") { JSON.generate([1], 5) }
gen("layout not a string") { JSON.generate([1], indent: 1) }

# pretty_generate takes them too
gen("pretty ascii") { JSON.pretty_generate({"a" => ["é"]}, ascii_only: true) }
gen("pretty script") { JSON.pretty_generate(["</"], script_safe: true) }
gen("pretty nesting") { JSON.pretty_generate([[[1]]], max_nesting: 2) }
gen("pretty nan") { JSON.pretty_generate([Float::NAN], allow_nan: true) }
gen("pretty layout") { JSON.pretty_generate([1], indent: "  ", space: " ", object_nl: "\n", array_nl: "\n") }
gen("pretty unknown") { JSON.pretty_generate([1], foo: 1) }

# and a builtin's #to_json, where anything but a Hash is no options
gen("to_json") { "é".to_json(ascii_only: true) }
gen("to_json hash") { {"a" => "/"}.to_json(script_safe: true) }
gen("to_json nan") { [Float::NAN].to_json(allow_nan: true) }
gen("to_json nesting") { [[1]].to_json(max_nesting: 1) }
gen("to_json unknown") { "x".to_json(foo: 1) }
gen("to_json nil") { "é".to_json(nil) }
gen("to_json int") { "x".to_json(5) }

# a #to_json of the program's own is its own text, escaped by nothing
class OwnText
  def to_json(*args) = "\"é/\""
end
gen("own text") { JSON.generate([OwnText.new], ascii_only: true, script_safe: true) }
gen("own text pretty") { JSON.pretty_generate([OwnText.new], ascii_only: true, script_safe: true) }
