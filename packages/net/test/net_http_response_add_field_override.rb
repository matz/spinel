# Parsed fields must reach the response's override after folding completes.
require "net/http"

$parsed_fields = []
class Net::HTTPOK
  def add_field(name, value)
    $parsed_fields << name.downcase + "=" + value
  end
end

reader, writer = IO.pipe
writer.write("HTTP/1.1 200 OK\r\nX-Item: first\r\n second\r\nx-item: third\r\n\r\nbody")
writer.close
buffered = Net::BufferedIO.new(reader)
response = Net::HTTPResponse.read_new(buffered)
p response.is_a?(Net::HTTPOK)
p $parsed_fields == ["x-item=first second", "x-item=third"]
p response["x-item"].nil?
p buffered.read_all.bytesize
reader.close

class ChangedFieldResponse < Net::HTTPResponse
  def add_field(name, value)
    super
    value.replace("changed")
  end
end

class ChangedFieldParser < Net::HTTPResponse
  def self.response_class(code)
    ChangedFieldResponse
  end
end

reader, writer = IO.pipe
writer.write("HTTP/1.1 200 OK\r\nX-Item: initial\r\n\r\n")
writer.close
response = ChangedFieldParser.read_new(Net::BufferedIO.new(reader))
p response["x-item"] == "changed"
copy = response["x-item"]
copy.clear
p response["x-item"] == "changed"
reader.close
