# spinel: gc-stress
$word = +"abc\0z"
$calls = 0
class Saved < StandardError
  def to_s
    $calls += 1
    $word
  end
end
def move
  $word.replace("m" * 300)
  "!"
end
def mutate
  $word.chop!
  "?"
end
begin
  raise Saved, "stored"
rescue StandardError => e
  text = "#{e}#{move}"
  p [text.bytesize, text == "m" * 300 + "!"]
  $word = +"abc\0z"
  p "#{e}" "#{mutate}".bytes
  $word = +"old"
  p "#{e}#{$word = +"new"}"
  s = +"prefix/"
  s << "#{e}#{mutate}"
  p s
end
p $calls
