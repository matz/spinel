p :first_symbol
def twice = [yield, yield(:head)]
p twice { |name = nil| name }
p twice { |name = nil| name.inspect }

def pair = [yield(1), yield(2, :x)]
p pair { |a, name = nil| name }

def klass = [yield, yield(Integer)]
p klass { |k = nil| k }

def text = [yield, yield("s")]
p text { |s = nil| s }

def num = [yield, yield(3)]
p num { |n = nil| n }
