# Runtime equality callbacks keep the String object they receive.
$callback_values = []
class StringKeyComparison
  def hash = "abc".hash
  def eql?(value)
    $callback_values << value
    false
  end
end
source = +"abc"
p [source, StringKeyComparison.new].uniq.size
p $callback_values.size
$callback_values[0] << "!"
p source

$callback_values = []
class StringPatternComparison
  def ==(value)
    $callback_values << value
    false
  end
end
source = +"abc"
p [source, nil].any?(StringPatternComparison.new)
p $callback_values.size
$callback_values[0] << "!"
p source

p [+"abc", "abc", nil].uniq.size
p [+"abc", nil].any?("abc")
