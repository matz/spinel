# A container return annotation also pins the instance variable it reads.
# External getter and direct ivar declarations must agree with that pin,
# whether it came from an ivar annotation or another method's return.
class ExplicitReader
  # @rbs @items: Array[String]

  def initialize
    @items = ["explicit"]
  end

  def items = @items
end

class ImplicitReader
  def initialize
    @items = ["implicit"]
  end

  #: () -> Array[String]
  def strings = @items

  def items = @items
end

class MemoReader
  #: () -> Array[String]
  def self.strings
    @items ||= ["memo"]
  end

  def self.items = @items
end

p ExplicitReader.new.items
reader = ImplicitReader.new
p reader.strings, reader.items
p MemoReader.strings, MemoReader.items

# Deferred override pins must also accept matching external ivar declarations.
class FamilyReader
  def initialize
    @rows = ["family"]
  end

  #: () -> Array[String]
  def rows = @rows
end
class FamilyReaderChild < FamilyReader
  #: () -> Array[String]
  def rows = @rows
end
[FamilyReader.new, FamilyReaderChild.new].each { |obj| p obj.rows }
p FamilyReaderChild.new.rows
