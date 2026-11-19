# JSON.parse's allow_trailing_comma, allow_nan and allow_control_characters,
# as json 2.18.0 (Ruby 4.0.9) reads them: each one lets in what it names and
# nothing more, a truthy value turns it on, and without it the same input is
# the error it was.
require "json"

def parse(text, **options)
  p JSON.parse(text, **options)
rescue JSON::ParserError => e
  puts "#{e.class}: #{e.message}"
end

comma = {allow_trailing_comma: true}
["[1,]", "[1 , ]", "[1,/*c*/]", "[[1,],]", "{\"a\":1,}", "{\"a\":1, }",
 "[1,,]", "[,]", "{,}", "{\"a\":1,,}", "[1,]]", "1,", "[]"].each do |text|
  parse(text)
  parse(text, **comma)
end
parse("[1,]", allow_trailing_comma: nil)
parse("[1,]", allow_trailing_comma: 1)
parse("[1,]", "allow_trailing_comma" => true)

nan = {allow_nan: true}
["NaN", "[NaN]", "[Infinity]", "[-Infinity]", "{\"a\":NaN}", "[NaNx]",
 "[Infinityx]", "[-Inf]", "[-NaN]", "[nan]", "[Infinit]", "[+Infinity]"].each do |text|
  parse(text)
  parse(text, **nan)
end
parse("[NaN]", allow_nan: false)
values = JSON.parse("[NaN, Infinity, -Infinity]", allow_nan: true)
p values.map(&:class), values.map(&:nan?), values.map(&:infinite?)

control = {allow_control_characters: true}
["[\"a\tb\"]", "[\"a\nb\"]", "[\"\x01\x1f\"]", "[\"a\x00b\"]", "{\"a\tb\":1}",
 "[\"a\tb\\n\"]", "[\"\\u00e9\t\"]", "[\"\\q\"]", "[\"\t\\\"\"]"].each do |text|
  parse(text)
  parse(text, **control)
end
p JSON.parse("{\"a\x00b\":1}", **control).keys.map(&:bytesize)

options = {allow_nan: [true, false].first, allow_trailing_comma: true}
p JSON.parse("[Infinity, -Infinity, NaN,]", options)
p JSON.parse("{\"a\tb\":NaN,}", allow_control_characters: true, allow_nan: true,
             allow_trailing_comma: true, symbolize_names: true)
begin
  JSON.parse("[[[1,],],]", allow_trailing_comma: true, max_nesting: 2)
rescue JSON::NestingError => e
  p e.message
end
