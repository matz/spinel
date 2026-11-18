# frozen_string_literal: true
require_relative 'string_returns'

class SelectedText
  #: (String, Integer) -> String?
  def answer(text, mode)
    begin
      raise "choose"
    rescue RuntimeError
      mode == 0 ? text : nil
    end
  end
end

class BoxedText
  def answer(text, mode)
    mode == 0 ? text : nil
  end
end

text = +"abc"
[SelectedText.new, BoxedText.new].each do |source|
  value = ReadText.read(source, text, 0)
  p value.equal?(text)
  p ReadText.read(source, text, 1)
end

class FreshText
  #: (String, Integer) -> String
  def answer(text, mode)
    text.dup
  ensure
    GC.start
  end
end

fresh = ReadText.read(FreshText.new, text, 0)
p [fresh, fresh.equal?(text)]
