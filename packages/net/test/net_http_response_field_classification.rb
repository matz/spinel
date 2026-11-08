# Enumerable classification must not call the field value's is_a? override.
require "net/http"

$classification_calls = 0
class FieldPieces
  include Enumerable

  def is_a?(type)
    $classification_calls += 1
    false
  end

  def each
    yield "alpha"
    yield "beta"
  end

  def to_s = "scalar"
end

class Net::HTTPOK
  def add_field(name, value)
    super(name, FieldPieces.new)
  end
end

reader, writer = IO.pipe
writer.write("HTTP/1.1 200 OK\r\nX-Pieces: ignored\r\n\r\nbody")
writer.close
buffered = Net::BufferedIO.new(reader)
response = Net::HTTPResponse.read_new(buffered)
p response["x-pieces"] == "alpha, beta"
p $classification_calls == 0
p buffered.read_all.bytesize
reader.close
