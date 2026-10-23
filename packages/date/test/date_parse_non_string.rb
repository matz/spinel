# spinel: share
# What the parsers raise for an argument that is not a String: a TypeError
# naming nil, true or false, or the class of anything else, as CRuby's
# StringValue does; an object with its own to_str is read through it.
require "date"

class DateText
  def to_str
    "2001-02-03"
  end
end

def show(label)
  p [label, yield]
rescue TypeError, Date::Error => e
  p [label, e.class, e.message]
end

[nil, true, false, 1, :a, DateText.new].each do |v|
  show("_parse") { Date._parse(v) }
  show("parse") { Date.parse(v).to_s }
  show("DateTime.parse") { DateTime.parse(v).to_s }
  show("_iso8601") { Date._iso8601(v) }
  show("_rfc3339") { Date._rfc3339(v) }
  show("_xmlschema") { Date._xmlschema(v) }
  show("_rfc2822") { Date._rfc2822(v) }
  show("_httpdate") { Date._httpdate(v) }
  show("_jisx0301") { Date._jisx0301(v) }
  show("iso8601") { Date.iso8601(v).to_s }
  show("DateTime.iso8601") { DateTime.iso8601(v).to_s }
  show("_strptime") { Date._strptime(v, "%Y") }
  show("strptime") { Date.strptime(v, "%Y-%m-%d").to_s }
  show("DateTime.strptime") { DateTime.strptime(v, "%Y-%m-%d").to_s }
  show("strptime format") { Date.strptime("2001", v).to_s }
  show("strftime") { Date.new(2001, 2, 3).strftime(v) }
end

# The same, written literally, so each argument keeps its own type.
show("_parse nil") { Date._parse(nil) }
show("parse nil") { Date.parse(nil).to_s }
show("DateTime.parse nil") { DateTime.parse(nil).to_s }
show("_strptime nil") { Date._strptime(nil, "%Y") }
show("strptime nil") { Date.strptime(nil, "%Y").to_s }
show("strftime nil") { Date.new(2001, 2, 3).strftime(nil) }
show("_parse true") { Date._parse(true) }
show("_parse 1") { Date._parse(1) }

# Through a parameter that holds a String or nil, as Time.parse passes it.
def via_parse(s) = Date._parse(s)
def via_strptime_hash(s) = Date._strptime(s, "%Y")
def via_strptime(s) = Date.strptime(s, "%Y").to_s
["2026-10-22", nil].each do |s|
  show("via _parse") { via_parse(s) }
  show("via _strptime") { via_strptime_hash(s) }
  show("via strptime") { via_strptime(s) }
end
