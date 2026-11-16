require "json"

options = [
  {foo: 1}, {allow_nan: true}, {allow_trailing_comma: true}, {allow_comments: true},
  {create_additions: false}, {freeze: false}, {freeze: true},
  {array_class: Array}, {array_class: nil}, {array_class: false},
  {decimal_class: Float}, {decimal_class: nil}, {decimal_class: false},
  {allow_control_characters: true}, {allow_invalid_escape: true},
  {"on_load" => nil}, {1 => true}
]
options.each { |opts| p JSON.parse('[1.5,{"s":"x"}]', **opts) }
p JSON.parse('[1.5,{"s":"x"}]', foo: 1, allow_comments: true, allow_duplicate_key: false)
p JSON.parse('[1.5,{"s":"x"}]', {foo: 1, create_additions: false, freeze: false})
