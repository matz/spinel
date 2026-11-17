# spinel: gc-stress
$calls = 0
$word = +"abc\0z"
class Caption
  def self.to_s
    $calls += 1
    $word
  end
end
def change
  $word.chop!
  "!"
end
p Caption.to_s.bytes
p "#{Caption}#{change}".bytes
$word = +"first"
p "#{Caption}" "#{change}"
$word = +"old"
p "#{Caption}#{$word = +"new"}"
p $calls
