require "stringio"

class DispatchSource
  def initialize(word)
    @word = word
  end

  def string = @word
end

class DispatchText < String
end

class DispatchMissing
  def string = nil
end

class DispatchFailure
  def string = raise("no string")
end

def observe_string(source, original)
  text = source.string
  if text
    p [text.class.name, text.equal?(original)]
    text << "!"
  else
    p text
  end
rescue RuntimeError => error
  p error.message
end

word = +"word"
fancy = DispatchText.new("fancy")
native = StringIO.new(word)
[DispatchSource.new(word), native, DispatchSource.new(fancy), DispatchMissing.new, DispatchFailure.new].each_with_index do |source, index|
  observe_string(source, index == 2 ? fancy : word)
end
p [word, fancy]


class HierarchyBase
  def self.text = "base"
  def text = self.class.text
end
class HierarchyChild < HierarchyBase
  def self.text = "child"
end
p HierarchyBase.new.text, HierarchyChild.new.text

class InspectTime < Time
  def inspect = sec == 0 ? 42 : "ok"
end
p [InspectTime.at(0), InspectTime.at(1), 1]

class StringReader
  def initialize = @value = +"a"
  def value = @value
  def value=(v)
    @value = v
  end
  def label = +"parent"
end

reader = StringReader.new
p(reader.value ||= "unused")
p(reader.value &&= +"assigned")
class DisplayReader
end
display = DisplayReader.new
p display.to_s.start_with?("#<DisplayReader:")
def display.to_s = "singleton"
p display.to_s
p(reader.label << "!")
class << reader
  def label = @value
end
reader.label << "!"
p reader.value

def call_result(method)
  method.call
end
p call_result(reader.method(:value)).equal?(reader.value)

boxed_reader = [reader, 1].first
bound_result = StringReader.instance_method(:value).bind_call(boxed_reader)
p bound_result.equal?(reader.value)
