# frozen_string_literal: true

def inspect_value(x)
  [x.class, x.object_id == x.object_id]
end
class VirtualSlot
  %w[cat dog].each do |word|
    define_method(word) { inspect_value(word) }
  end
end
class VirtualReader
  ["cat", "dog"].each do |word|
    define_method(word) { [word, word.frozen?, word.class, word.bytesize, word.upcase, word.equal?(word)] }
  end
end
p VirtualSlot.new.cat, VirtualSlot.new.dog
p VirtualReader.new.cat, VirtualReader.new.dog
