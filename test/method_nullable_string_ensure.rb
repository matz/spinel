# spinel: share
class SelectText
  def choose(text, missing, replacement: nil)
    text = replacement unless replacement.nil?
    begin
      return missing ? nil : text
    ensure
      GC.start
      scratch = +"ensure"
      scratch << "!"
    end
  end
end

class TextChild < SelectText
end

def choose(reader, text, missing)
  TextChild.instance_method(:choose).bind(reader).call(text, missing, replacement: text)
end

reader = TextChild.new
text = +"selected"
p choose(reader, text, true)
p choose(reader, text, false).equal?(text)
p reader.method(:choose).call(text, true, replacement: text)
