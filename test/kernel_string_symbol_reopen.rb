$trace = []

def typed
  String(:abc)
end

def boxed(value)
  String(value)
end

def input(index)
  $trace << :argument
  [:abc, :fallback, 12][index]
end

class Symbol
  alias original_text to_s
end

p [typed, boxed(input(0)), $trace.dup]

class Symbol
  def to_s
    $trace << :to_s
    +"explicit"
  end
end

p [typed, boxed(input(0)), $trace.dup]
p [:abc.to_s, input(0).to_s, :abc.original_text]

class Symbol
  def to_str
    $trace << :to_str
    self == :fallback ? nil : +"implicit"
  end
end

p [typed, boxed(input(0)), boxed(input(1)), boxed(input(2)), $trace.dup]
p [String(nil), String("literal")]

class Unrelated
  def String(value)
    [value, value.to_s]
  end
end

p Unrelated.new.String(12)
