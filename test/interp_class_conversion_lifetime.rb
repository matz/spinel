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

class Révision
end
class Plain
end
def binary_tail = "x".b
def text_tail = "x"
p "#{Révision}#{binary_tail}".encoding.name
p "#{Plain}#{binary_tail}".encoding.name
p "#{Révision}#{text_tail}".encoding.name
p "#{Plain}#{text_tail}".encoding.name

$word = "x".b
p "#{Caption}#{text_tail}".encoding.name
$word = "x"
p "#{Caption}#{binary_tail}".encoding.name
p $calls
