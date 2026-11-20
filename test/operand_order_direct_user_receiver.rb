class OrderPoint
  def initialize(value) = (@value = value)
  def format(argument) = "#{@value}|#{argument}"
end

point = OrderPoint.new(3)
p point.format((point = OrderPoint.new(8); "q"))

class OrderText
  attr_reader :text
  def initialize(text) = (@text = text)
  def combine(argument) = [@text, argument]
end

class OrderCaller
  def initialize
    @item = OrderText.new(+"retained")
  end

  def replace
    @item.text << "!"
    @item = OrderText.new(+"replacement")
    GC.start
    "argument"
  end

  def result = @item.combine(replace)
end

p OrderCaller.new.result
