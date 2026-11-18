require "stringio"

$calls = 0
class RuntimeError
  def to_s
    $calls += 1
    "custom"
  end
end

io = StringIO.new
io.print(RuntimeError.new("stored"))
p io.string, $calls
