# spinel: not-cruby -- json 2.18.0 answers the run before such an escape twice ("a\<TAB>" reads as "aa\<TAB>"); this follows json 2.19's fix.
# With allow_control_characters a raw control character is taken, but one
# right after a backslash is still an invalid escape.
require "json"

["[\"a\\\tb\"]", "[\"a\\\nb\"]", "[\"\\\x01\"]", "{\"\\\t\":1}"].each do |text|
  begin
    p JSON.parse(text, allow_control_characters: true)
  rescue JSON::ParserError => e
    puts e.message
  end
end
