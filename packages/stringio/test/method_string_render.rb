require "stringio"

class RenderText < String
end

class RenderSource
  def to_s
    $render_calls += 1
    RenderText.new("hi\0tail")
  end
end

$render_calls = 0
source = RenderSource.new
stream = StringIO.new
stream.print(source)
p stream.string
p $render_calls
