# spinel: not-cruby -- Diagnostic positions follow CRuby 4.0.7's json 2.18.0; newer json gems differ.
require "json"

def duplicate(text, **options)
  begin
    p JSON.parse(text, **options)
  rescue JSON::ParserError => error
    puts error.message
  end
end

duplicate('{"x":1,"x":2}', allow_duplicate_key: false)
duplicate('{"x":1,"x":2}', allow_duplicate_key: false)
duplicate('{"x":1,"x":2}', allow_duplicate_key: nil)
duplicate('{"x":1,"x":2}', allow_duplicate_key: true)
duplicate('{"x":null,"x":false}', allow_duplicate_key: false)
duplicate('{"x":1,"\\u0078":2}', symbolize_names: true, allow_duplicate_key: false)
duplicate('{"a\\n":1,"a\\n":2}', allow_duplicate_key: false)
begin
  JSON.parse('{"%s%s":1,"%s%s":2}', allow_duplicate_key: false)
rescue JSON::ParserError => error
  # json 2.18.0's message treats the key as a format string; assert the stable class.
  puts error.class
end
duplicate("[\n  {\"é\":1,\"é\":2}\n]", allow_duplicate_key: false)
duplicate('{"a":0,"b":1,"b":2,"a":3}', allow_duplicate_key: false)
duplicate('{"a":1,"b":[{"a":2},{"a":3}]}', allow_duplicate_key: false)
duplicate('[{"a":1},{"a":2}]', allow_duplicate_key: false)
duplicate('{"x":{"y":null,"y":2}}', allow_duplicate_key: false)
duplicate('{"":1,"":2}', allow_duplicate_key: false)
duplicate('{"x":1,"x":2}', allow_duplicate_key: 0)
duplicate('{"x":1,"x":2}', allow_duplicate_key: false)
duplicate('["é",{"x":1,"x":2}]', allow_duplicate_key: false)
duplicate('{"a\\u0000b":1,"a\\u0000b":2}', allow_duplicate_key: false)
