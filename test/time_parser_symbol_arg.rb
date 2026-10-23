# The fixed-format Time parsers match their regexp against a Symbol, so a
# Symbol argument reads its name, as under CRuby; Time.parse still raises
# TypeError for one.
require "time"
p Time.xmlschema(:"2024-02-29T00:00:00Z")
p Time.iso8601(:"2024-02-29T01:02:03+09:00")
p Time.rfc2822(:"Thu, 29 Feb 2024 00:00:00 +0000")
p Time.httpdate(:"Thu, 29 Feb 2024 00:00:00 GMT")
s = [:"2024-02-29T00:00:00Z", 1][0]
p Time.xmlschema(s)
p((Time.parse(:"2024-02-29") rescue $!.class))
p((Time.xmlschema(5) rescue $!.class))
p((Time.httpdate(5) rescue $!.class))
p((Time.xmlschema(:bogus) rescue $!.message))
p((Time.rfc2822(:"no date") rescue $!.message))
p((Time.httpdate(:bogus) rescue $!.message))
p((Time.xmlschema("bogus") rescue $!.message))

# a Symbol whose fields are out of range raises before the format message;
# a later bad String still shows the String
begin
  Time.xmlschema(:"2024-13-45T00:00:00Z")
rescue ArgumentError
  p :raised
end
begin
  Time.xmlschema("bogus")
rescue ArgumentError => e
  p e.message
end
