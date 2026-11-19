# spinel: gc-minor
# Choose the store's key before to_str changes the receiver's bytes or width.

class StoreReplacement
  def to_str
    $calls += 1
    $text.replace($replacement)
    "Z"
  end
end

def reset(replacement)
  $text = +"abc"
  $calls = 0
  $replacement = replacement
end

reset("m" * 40 + "c")
receiver = [$text, 7][0]
receiver["b"] = StoreReplacement.new
p [$text, $calls]

reset("m" * 40)
receiver = [$text, 7][0]
begin
  receiver[7..9] = StoreReplacement.new
rescue RangeError => e
  p [e.message, $calls]
end

reset("q" * 10)
receiver = [$text, 7][0]
receiver[0..99] = StoreReplacement.new
p [$text, $calls]

reset("m" * 40)
receiver = [$text, 7][0]
receiver[-2, 1] = StoreReplacement.new
p [$text, $calls]

reset("m")
receiver = [$text, 7][0]
begin
  receiver["c"] = StoreReplacement.new
rescue IndexError => e
  p [e.message, $calls]
end

reset("ΩΩ")
receiver = [$text, 7][0]
receiver[/c/] = StoreReplacement.new
p [$text.bytes, $calls]

reset("ΩΩ")
receiver = [$text, 7][0]
receiver["c"] = StoreReplacement.new
p [$text.bytes, $calls]

reset("ΩΩ")
receiver = [$text, 7][0]
receiver[2..2] = StoreReplacement.new
p [$text.bytes, $calls]

reset("ΩΩ")
receiver = [$text, 7][0]
receiver[2, 1] = StoreReplacement.new
p [$text.bytes, $calls]

text = +"aé"
receiver = [text, 7][0]
begin
  receiver[9, 1] = "ö".b
rescue Encoding::CompatibilityError => e
  p [e.class, text]
end
