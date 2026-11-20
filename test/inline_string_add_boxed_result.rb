# frozen_string_literal: true

class Item
  def inspect = "item"
end

def combine(a, b, &block)
  block.call(a) + b
end

def replace_text
  @text = +"new"
  +"first"
end

def report
  @text = +"old"
  @item = Item.new
  result = begin
    combine(replace_text, @text, &:upcase).inspect
  rescue IOError
    "raised"
  end
  p [result, @text]
end

def report_with_block_expression
  result = combine(($events << 1; +"first"), ($events << 2; +"last"), &($events << 3; :upcase)).inspect
  p [result, $events]
end

$events = []
report
report_with_block_expression
