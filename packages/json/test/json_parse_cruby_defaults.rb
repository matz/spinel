# spinel: not-cruby -- These defaults follow CRuby 4.0.7's bundled json 2.18.0; newer json gems differ.
require "json"
p JSON.parse('{"a":1,"a":2}')
p JSON.parse('{"a":1,"a":2}', {})
p JSON.parse('{"a":1,"a":2}', nil)
p JSON.parse('[{"a":1,"a":2},{"a":3}]')
p JSON.parse('"\uDC00"').bytes
p JSON.parse('{"\uDC00":"\uDFFF"}').keys.first.bytes
p JSON.parse('["\uD800\uDC00"]').first.bytes
p JSON.parse('"\uDC00"', allow_duplicate_key: false).bytes
p JSON.parse('"\uDC00"', allow_duplicate_key: true).bytes
begin
  JSON.parse('{"a":1,"a":2}', {allow_duplicate_key: false})
rescue JSON::ParserError => error
  puts error.message
end
