# Parse only the response head; retain body bytes and subsequent responses.
require "net/http"

def parse_head(data)
  reader, writer = IO.pipe
  writer.write(data)
  writer.close
  buffered = Net::BufferedIO.new(reader)
  begin
    response = Net::HTTPResponse.read_new(buffered)
    puts response.class
    p [response.http_version == "1.1", response.code == "200", response.message == "OK"]
    p [response["x-test"] == "one, two", response.get_fields("X-Test") == ["one", "two"]]
    p response["x-fold"] == "first second"
    begin
      p response.body.nil?
    rescue IOError => error
      puts error.class
    end
  rescue StandardError => error
    puts error.class
  end
  p buffered.read_all.bytesize
  reader.close
end

parse_head("HTTP/1.1 200 OK\r\nX-Test: one\r\nx-test: two\r\nX-Fold: first\r\n\tsecond\r\n\r\nbody")
parse_head("http/1.1 200 OK\r\n\r\n")
parse_head("HTTP 200\r\n\r\n")
parse_head("HTTP/1.0 404 Not Found\r\n\r\n")
parse_head("HTTP/1.1 299 Extra\r\n\r\n")
parse_head("HTTP/1.1 599 Extra\r\n\r\n")
parse_head("HTTP/1.1 999 Extra\r\n\r\n")
parse_head("HTTP/1.1 20 OK\r\n\r\n")
parse_head("garbage\r\n\r\n")
parse_head("")
parse_head("HTTP/1.1 200 OK\r\nBroken\r\n\r\n")
parse_head("HTTP/1.1 200 OK\r\nX-Test: one\r\n")
parse_head("HTTP/1.1 200 OK \t\r\n\r\n")
parse_head("HTTP/1.1 200 OK\nX-Test : one\nX-Test: two\n\n")
parse_head("HTTP/1.1 200 OK\r\n first\r\n\r\n")
parse_head("HTTP/1.1 200 OK\r\n\tfirst\r\n\r\n")

reader, writer = IO.pipe
writer.write("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n")
writer.close
buffered = Net::BufferedIO.new(reader)
first = Net::HTTPResponse.read_new(buffered)
second = Net::HTTPResponse.read_new(buffered)
p [first.is_a?(Net::HTTPInformation), second.is_a?(Net::HTTPOK)]
reader.close
