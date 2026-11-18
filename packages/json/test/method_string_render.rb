require "json"

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
p source.to_s
p JSON.generate(source)
p $render_calls
