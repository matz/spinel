# spinel: share
$cleanups = 0
class SelectText
  def choose(text, missing, replacement: nil)
    text = replacement unless replacement.nil?
    begin
      return missing ? nil : text
    ensure
      $cleanups += 1
      GC.start
      scratch = +"ensure"
      scratch << "!"
    end
  end
end

class TextChild < SelectText
end

def choose(reader, text, missing, replacement)
  TextChild.instance_method(:choose).bind(reader).call(text, missing, replacement: replacement)
end

reader = TextChild.new
text = +"selected"
replacement = +"replacement"
p choose(reader, text, true, replacement)
p choose(reader, text, false, replacement).equal?(replacement)
p reader.method(:choose).call(text, true, replacement: text)
p $cleanups

def pending_text(missing)
  return missing ? nil : +"pending"
ensure
  GC.start
  scratch = +"ensure"
  scratch << "!"
end

p pending_text(false)
p pending_text(true)
