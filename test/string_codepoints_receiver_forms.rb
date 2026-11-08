# Receiver shape, binary encoding and allocation checks for codepoint arrays.
def returned_text
  String.new("é\0🙂")
end

text = returned_text.freeze
puts text.codepoints == [233, 0, 128578]
copy = text
puts copy.codepoints == [233, 0, 128578]
puts text.send(:codepoints) == [233, 0, 128578]
puts text.each_codepoint.to_a == [233, 0, 128578]
puts text.send(:each_codepoint).to_a == [233, 0, 128578]
boxed = [text, 7].first
puts boxed.codepoints == [233, 0, 128578]
puts boxed.each_codepoint.to_a == [233, 0, 128578]
puts "é\0🙂".b.codepoints == [195, 169, 0, 240, 159, 153, 130]
puts "é\0🙂".b.each_codepoint.to_a == [195, 169, 0, 240, 159, 153, 130]
puts "é\0🙂".b.each_codepoint.to_a == "é\0🙂".b.bytes
puts "x\0y"[1].codepoints == [0]
puts String.new("é\0🙂").codepoints == [233, 0, 128578]
puts ("é\0🙂" * 1000).codepoints.length == 3000

class CodepointSource
  def codepoints
    [7]
  end
end
puts CodepointSource.new.codepoints == [7]
class CodepointText < String
  def own_points
    self.codepoints
  end
end
class CustomCodepointText < String
  def codepoints
    [8]
  end
end
puts CodepointText.new("é\0🙂").codepoints == [233, 0, 128578]
puts CodepointText.new("é\0🙂").own_points == [233, 0, 128578]
puts CustomCodepointText.new("é\0🙂").codepoints == [8]
maybe_nil = nil
maybe_nil = text if ARGV.size > 5
begin
  maybe_nil.codepoints
rescue NoMethodError
  puts true
end
