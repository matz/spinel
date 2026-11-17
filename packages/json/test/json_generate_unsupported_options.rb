# spinel: not-cruby -- Result-changing unsupported options raise by name; CRuby lays out, converts or checks instead.
require "json"
# JSON.generate, pretty_generate and a builtin's #to_json refuse an option
# Spinel would otherwise ignore while the answer of Ruby 4.0's bundled json
# (2.18) changes, by name, as JSON.parse does. A layout option set to what
# the call writes anyway, a false strict or as_json, and a true
# allow_duplicate_key still work.

def gen(label)
  puts "#{label}: #{yield}"
rescue ArgumentError => e
  puts "#{label}: #{e.message}"
end

gen("indent") { JSON.generate([1], indent: "\t") }
gen("space") { JSON.generate({"a" => 1}, space: " ") }
gen("space_before") { JSON.generate({"a" => 1}, space_before: " ") }
gen("object_nl") { JSON.generate({"a" => 1}, object_nl: "\n") }
gen("array_nl") { JSON.generate([1], array_nl: "\n") }
gen("strict") { JSON.generate([1], strict: true) }
gen("as_json") { JSON.generate([1], as_json: ->(x) { x }) }
gen("depth") { JSON.generate([1], depth: 2) }
gen("allow_duplicate_key") { JSON.generate({"a" => 1}, allow_duplicate_key: false) }
gen("allow_duplicate_key nil") { JSON.generate({"a" => 1}, allow_duplicate_key: nil) }
gen("pretty indent") { JSON.pretty_generate([1], indent: "\t") }
gen("pretty no indent") { JSON.pretty_generate([1], indent: nil) }
gen("pretty space_before") { JSON.pretty_generate({"a" => 1}, space_before: " ") }
gen("to_json indent") { [1].to_json(indent: "  ") }

gen("generate defaults") { JSON.generate({"a" => [1]}, indent: "", space: "", strict: false, allow_duplicate_key: true) }
gen("pretty defaults") { JSON.pretty_generate({"a" => [1]}, indent: "  ", space: " ", object_nl: "\n", array_nl: "\n", space_before: "") }
