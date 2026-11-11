# Fields inserted by a response hook use HTTPHeader's conversion and append rules.
require "net/http"

class HeaderText
  def to_s = "converted"
  def to_str = raise "unexpected to_str"
end

class FieldValueResponse < Net::HTTPResponse
  def add_field(name, value)
    super(name, [value, [HeaderText.new, nil]])
  end
end

class FieldValueParser < Net::HTTPResponse
  def self.response_class(code) = FieldValueResponse
end

reader, writer = IO.pipe
writer.write("HTTP/1.1 200 OK\r\nX-Test: first\r\n\r\nbody")
writer.close
buffered = Net::BufferedIO.new(reader)
response = FieldValueParser.read_new(buffered)
p response.get_fields("x-test") == ["first", "converted", ""]
p response["x-test"] == "first, converted, "
p buffered.read_all == "body"
reader.close

begin
  response.add_field("X-Test", "bad\nvalue")
rescue ArgumentError => error
  puts error.class
end
p response.get_fields("x-test") == ["first", "converted", ""]

plain = Net::HTTPOK.new("1.1", "200", "OK")
plain.add_field("X-Test", "first")
begin
  plain.add_field("X-Test", ["second", "bad\rvalue"])
rescue ArgumentError => error
  puts error.class
end
p plain.get_fields("x-test") == ["first", "second"]
begin
  plain.add_field("X-New", ["good", "bad\nvalue"])
rescue ArgumentError => error
  puts error.class
end
p plain.get_fields("x-new").nil?
plain.add_field("X-Empty", [])
p plain["x-empty"] == ""
plain.add_field("X-Nil", nil)
p plain["x-nil"] == ""
plain.freeze
plain.add_field("X-Frozen", "yes".freeze)
p plain["x-frozen"] == "yes"
