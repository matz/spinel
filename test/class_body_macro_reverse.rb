# A class-body macro can reverse an array to build a method name while
# preserving the original array for later use.
module ReverseNames
  def reversed_constant(label, parts)
    const_set(label, public_send(parts.reverse.join("_")))
    const_set(:ORIGINAL_FIRST, parts.first)
  end
end

class ReverseNamed
  extend ReverseNames
  def self.calc_box_size = 42
  reversed_constant :SIZE, ["size", "box", "calc"]
end

p ReverseNamed::SIZE, ReverseNamed::ORIGINAL_FIRST
