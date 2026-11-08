# Status, class selection and header iteration dispatch on the receiving class.
require "net/http"

$response_constructed = false
class Net::HTTPNotFound
  def initialize(version, code, message)
    $response_constructed = true
    super
  end
end

class SelectedHead < Net::HTTPResponse
  def self.response_class(code)
    Net::HTTPNotFound
  end
end

reader, writer = IO.pipe
writer.write("HTTP/1.1 200 OK\r\n\r\n")
writer.close
response = SelectedHead.read_new(Net::BufferedIO.new(reader))
p [response.is_a?(Net::HTTPNotFound), response.code == "200"]
reader.close

$response_constructed = false
class SuppliedHead < Net::HTTPResponse
  def self.read_status_line(sock)
    ["1.0", "404", "Missing"]
  end

  def self.each_response_header(sock)
    p $response_constructed
    yield "X-Supplied", "yes"
  end
end

reader, writer = IO.pipe
writer.write("body")
writer.close
buffered = Net::BufferedIO.new(reader)
response = SuppliedHead.read_new(buffered)
p [response.is_a?(Net::HTTPNotFound), response.http_version == "1.0", response.message == "Missing"]
p response["x-supplied"] == "yes"
p buffered.read_all.bytesize
reader.close
