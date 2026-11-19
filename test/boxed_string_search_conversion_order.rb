# spinel: gc-minor
# The offset and needle conversions can both change the receiver.

class SearchNeedle
  def to_str
    $calls << :needle
    $text.replace($replacement) unless $replacement.nil?
    "Z"
  end
end

class SearchOffset
  def to_int
    $calls << :offset
    $text.replace("x")
    3
  end
end

$replacement = "Z" + "x" * 40 + "Z"
$calls = []
$text = +"abc"
receiver = [$text, 7][0]
p [receiver.index(SearchNeedle.new), $calls]

$calls = []
$text = +"abc"
receiver = [$text, 7][0]
p [receiver.rindex(SearchNeedle.new, 70), $calls]

$calls = []
$text = +"abc"
receiver = [$text, 7][0]
p [receiver.index(SearchNeedle.new, 70), $calls]

$replacement = nil
$calls = []
$text = +"abcdef"
receiver = [$text, 7][0]
p [receiver.index(SearchNeedle.new, SearchOffset.new), $calls]
